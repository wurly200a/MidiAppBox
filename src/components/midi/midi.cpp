// MIDI OUT native 実装(実機側)。Phase 8b。詳細は midi.hpp / shared/hostapi_defs.h 参照。
#include "midi.hpp"

#include "board_pins.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hostapi_defs.h"

#include <cstring>

namespace midi {
namespace {

constexpr const char* TAG = "MIDI";
constexpr uart_port_t kMidiUart = UART_NUM_1;
constexpr size_t kMaxMsgLen = 8;

bool s_uart_ready = false;

// UART TX の直列化について(Phase 11 で確認):
// esp_timer タスク(クロック / L0 ディスパッチャ)と wasm アプリタスク
// (hostapi_midi_send)が同時に送出しうるが、IDF の uart_write_bytes は
// ドライバ内部の tx_mux で直列化されるため、メッセージのバイトが交錯する
// ことはない。したがって呼び出し側でロックは取らない(portMUX の臨界区間で
// uart_write_bytes を呼ぶのは、内部でセマフォを待つため不正でもある)。

// ---- MIDI IN 受信リングバッファ(Phase 9a) ----
// 生産者は UART イベントタスク(rx_task)、消費者は Midi_Recv(wasm アプリ
// スレッドから hostapi_midi_recv 経由)。臨界区間は短い(最大 256 レコードの
// memcpy)ので spinlock(既存の入力イベントキュー(hostapi.cpp)と同じ設計)。
constexpr int kRxUartBufSize = 1024;  // SysEx を考慮(Phase 8c で 302 バイトを確認済み)
constexpr int kRxUartQueueLen = 16;
constexpr int kRxRecQueueDepth = 256; // 4KB。SysEx 1件を余裕を持って収容
QueueHandle_t s_rx_uart_queue = nullptr;

hostapi_midi_recv_t s_rxq[kRxRecQueueDepth];
int s_rxq_head = 0;
int s_rxq_count = 0;
portMUX_TYPE s_rxq_mux = portMUX_INITIALIZER_UNLOCKED;

void rxq_reset()
{
    portENTER_CRITICAL(&s_rxq_mux);
    s_rxq_head = 0;
    s_rxq_count = 0;
    portEXIT_CRITICAL(&s_rxq_mux);
}

void rxq_push(uint8_t byte, uint64_t timestamp_us)
{
    bool dropped = false;
    portENTER_CRITICAL(&s_rxq_mux);
    if (s_rxq_count == kRxRecQueueDepth) { // 満杯: 最古を捨てる
        s_rxq_head = (s_rxq_head + 1) % kRxRecQueueDepth;
        s_rxq_count--;
        dropped = true;
    }
    hostapi_midi_recv_t& rec = s_rxq[(s_rxq_head + s_rxq_count) % kRxRecQueueDepth];
    rec.timestamp_us = timestamp_us;
    rec.byte = byte;
    memset(rec._reserved, 0, sizeof(rec._reserved));
    s_rxq_count++;
    portEXIT_CRITICAL(&s_rxq_mux);

    if (dropped) ESP_LOGW(TAG, "MIDI RX: ring buffer full, dropped oldest record");
}

// MIDI IN 受信タスク(Phase 9a)。UART イベントキューを監視し、UART_DATA は
// 受信バイトをタイムスタンプ付きでリングバッファへ積む。パースは一切行わ
// ない。1 回の UART イベントにまとまった複数バイトは同一の代表時刻を持つ
// (詳細は shared/hostapi_defs.h の "midi" セクション参照)。
void rx_task(void*)
{
    uart_event_t event;
    static uint8_t buf[kRxUartBufSize]; // タスク専用、スタックを圧迫しないよう static
    for (;;) {
        if (xQueueReceive(s_rx_uart_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        switch (event.type) {
        case UART_DATA: {
            int n = uart_read_bytes(kMidiUart, buf, event.size, 0);
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            for (int i = 0; i < n; ++i) rxq_push(buf[i], now_us);
            break;
        }
        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "MIDI RX: FIFO overflow");
            uart_flush_input(kMidiUart);
            xQueueReset(s_rx_uart_queue);
            break;
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "MIDI RX: ring buffer full");
            uart_flush_input(kMidiUart);
            xQueueReset(s_rx_uart_queue);
            break;
        case UART_FRAME_ERR:
            ESP_LOGW(TAG, "MIDI RX: framing error");
            break;
        case UART_PARITY_ERR:
            ESP_LOGW(TAG, "MIDI RX: parity error");
            break;
        default:
            break;
        }
    }
}

} // namespace

void Midi_Init()
{
    const uart_config_t cfg = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(kMidiUart, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(kMidiUart, PIN_MIDI_TX, PIN_MIDI_RX,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        // rx_buffer_size は SysEx を考慮し 1024(Phase 8c 検証済み設定を踏襲)。
        // event queue はオーバーラン/フレーミングエラー検知と MIDI IN 受信
        // (Phase 9a、rx_task が消費する)に使う。
        err = uart_driver_install(kMidiUart, /*rx_buffer_size=*/kRxUartBufSize,
                                   /*tx_buffer_size=*/0, kRxUartQueueLen,
                                   &s_rx_uart_queue, 0);
    }
    if (err == ESP_OK) {
        // MIDI は TX 側のみ論理反転信号(Phase 8a で確認済み)。RX 側は
        // TLP2361 がトーテムポール出力かつ反転型で、MIDI のカレントループ
        // 論理(アイドル = 無電流 = 出力 H)がそのまま UART の論理レベルに
        // 一致するため UART_SIGNAL_RXD_INV は付けない(board_pins.hpp 参照、
        // Phase 8c で実機検証済み)。
        err = uart_set_line_inverse(kMidiUart, UART_SIGNAL_TXD_INV);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Midi_Init failed: %s", esp_err_to_name(err));
        return;
    }
    s_uart_ready = true;
    ESP_LOGI(TAG, "MIDI OUT ready (GPIO%d, 31250bps, TXD inverted)", (int)PIN_MIDI_TX);

    xTaskCreate(rx_task, "midi_rx", 3072, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "MIDI IN ready (GPIO%d, RX not inverted)", (int)PIN_MIDI_RX);
}

int32_t Midi_Send(const uint8_t* bytes, size_t len)
{
    if (!s_uart_ready || bytes == nullptr || len == 0 || len > kMaxMsgLen) return -1;
    uart_write_bytes(kMidiUart, reinterpret_cast<const char*>(bytes), len);
    return 0;
}

void Midi_TxBytes(const uint8_t* bytes, size_t len)
{
    if (!s_uart_ready || bytes == nullptr || len == 0 || len > kMaxMsgLen) return;
    uart_write_bytes(kMidiUart, reinterpret_cast<const char*>(bytes), len);
}

void Midi_Reset()
{
    rxq_reset();
}

int32_t Midi_Recv(void* buf, size_t buf_len)
{
    const size_t max_records = buf_len / sizeof(hostapi_midi_recv_t);
    auto* out = static_cast<hostapi_midi_recv_t*>(buf);
    int32_t n = 0;

    portENTER_CRITICAL(&s_rxq_mux);
    while ((size_t)n < max_records && s_rxq_count > 0) {
        out[n] = s_rxq[s_rxq_head];
        s_rxq_head = (s_rxq_head + 1) % kRxRecQueueDepth;
        s_rxq_count--;
        n++;
    }
    portEXIT_CRITICAL(&s_rxq_mux);
    return n;
}

} // namespace midi
