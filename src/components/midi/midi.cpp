// MIDI OUT native 実装(実機側)。Phase 8b。詳細は midi.hpp / shared/hostapi_defs.h 参照。
#include "midi.hpp"

#include "board_pins.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace midi {
namespace {

constexpr const char* TAG = "MIDI";
constexpr uart_port_t kMidiUart = UART_NUM_1;
constexpr size_t kMaxMsgLen = 8;
constexpr uint32_t kClockPpqn = 24;
constexpr uint64_t kMinClockIntervalUs = 500; // 安全弁(異常値での高頻度化を防ぐ)

// ---- MIDI IN 受信バイトダンプ(Phase 8c、検証専用。パースは行わない) ----
constexpr int kRxBufSize = 1024;    // SysEx を考慮したリングバッファ(指示書どおり)
constexpr int kRxQueueLen = 16;
QueueHandle_t s_rx_event_queue = nullptr;

bool s_uart_ready = false;

// ---- 24ppqn クロック生成(既存クリックスケジューラからの通知で駆動) ----
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_clock_running = false;      // hostapi_midi_send の Start/Stop で切替
uint32_t s_last_target_ms = 0;     // 直近に受け取った予約時刻(発火有無は問わない、0=未確定)
uint32_t s_next_period_ms = 0;     // staging: 次に使う予測テンポ(0=未確定)
esp_timer_handle_t s_clock_timer = nullptr;

void clock_timer_cb(void*)
{
    if (!s_uart_ready) return;
    const uint8_t clock_byte = 0xF8;
    uart_write_bytes(kMidiUart, reinterpret_cast<const char*>(&clock_byte), 1);
}

void clock_timer_ensure()
{
    if (s_clock_timer) return;
    esp_timer_create_args_t args = {};
    args.callback = clock_timer_cb;
    args.name = "midi_clock";
    args.dispatch_method = ESP_TIMER_TASK;
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_clock_timer));
}

// s_mux の外(critical section の外)から呼ぶこと(esp_timer_stop は
// 内部で自身の排他制御を持ち、portMUX のクリティカルセクション内から
// 呼んではいけない)。
void clock_timer_stop()
{
    if (s_clock_timer) esp_timer_stop(s_clock_timer);
}

// MIDI IN 受信ダンプタスク(Phase 8c 検証専用)。UART イベントキューを
// 監視し、UART_DATA は受信バイトを 1 バイト 1 行の 16 進+タイムスタンプで
// ログ出力、それ以外(オーバーラン/フレーミングエラー等)はイベント種別を
// ログ出力する。パースは一切行わない。
void rx_dump_task(void*)
{
    uart_event_t event;
    static uint8_t buf[kRxBufSize]; // タスク専用、スタックを圧迫しないよう static
    for (;;) {
        if (xQueueReceive(s_rx_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        switch (event.type) {
        case UART_DATA: {
            int n = uart_read_bytes(kMidiUart, buf, event.size, 0);
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            for (int i = 0; i < n; ++i) {
                ESP_LOGI(TAG, "MIDI RX: %02X t=%u", buf[i], (unsigned)now_ms);
            }
            break;
        }
        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "MIDI RX: FIFO overflow");
            uart_flush_input(kMidiUart);
            xQueueReset(s_rx_event_queue);
            break;
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "MIDI RX: ring buffer full");
            uart_flush_input(kMidiUart);
            xQueueReset(s_rx_event_queue);
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
        // rx_buffer_size は SysEx を考慮し 1024(Phase 8c 指示書どおり)。
        // event queue はオーバーラン/フレーミングエラー検知に使う
        // (Phase 8c、rx_dump_task が消費する)。
        err = uart_driver_install(kMidiUart, /*rx_buffer_size=*/kRxBufSize,
                                   /*tx_buffer_size=*/0, kRxQueueLen,
                                   &s_rx_event_queue, 0);
    }
    if (err == ESP_OK) {
        // MIDI は TX 側のみ論理反転信号(Phase 8a で確認済み)。RX 側は
        // TLP2361 がトーテムポール出力かつ反転型で、MIDI のカレントループ
        // 論理(アイドル = 無電流 = 出力 H)がそのまま UART の論理レベルに
        // 一致するため UART_SIGNAL_RXD_INV は付けない(board_pins.hpp 参照、
        // Phase 8c 指示書の必須確認事項)。実機検証で RXD_INV ありでも同じ
        // ノイズパターンが再現することを確認済み(反転設定が原因ではないと
        // 実証済み、docs/dev-log.md Phase 8c 参照)。
        err = uart_set_line_inverse(kMidiUart, UART_SIGNAL_TXD_INV);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Midi_Init failed: %s", esp_err_to_name(err));
        return;
    }
    clock_timer_ensure();
    s_uart_ready = true;
    ESP_LOGI(TAG, "MIDI OUT ready (GPIO%d, 31250bps, TXD inverted)", (int)PIN_MIDI_TX);

    xTaskCreate(rx_dump_task, "midi_rx_dump", 3072, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "MIDI IN dump ready (GPIO%d, RX not inverted)", (int)PIN_MIDI_RX);
}

int32_t Midi_Send(const uint8_t* bytes, size_t len)
{
    if (!s_uart_ready || bytes == nullptr || len == 0 || len > kMaxMsgLen) return -1;

    if (len == 1) {
        if (bytes[0] == 0xFA || bytes[0] == 0xFB) { // Start / Continue
            portENTER_CRITICAL(&s_mux);
            s_clock_running = true;
            s_last_target_ms = 0;
            s_next_period_ms = 0;
            portEXIT_CRITICAL(&s_mux);
        } else if (bytes[0] == 0xFC) { // Stop
            portENTER_CRITICAL(&s_mux);
            s_clock_running = false;
            s_last_target_ms = 0;
            s_next_period_ms = 0;
            portEXIT_CRITICAL(&s_mux);
            clock_timer_stop();
        }
    }

    uart_write_bytes(kMidiUart, reinterpret_cast<const char*>(bytes), len);
    return 0;
}

void Midi_NotifyBeatScheduled(uint32_t target_ms)
{
    portENTER_CRITICAL(&s_mux);
    if (s_clock_running) {
        // 「直前に発音した時刻」ではなく「直前に受け取った予約時刻」との
        // 差分を使う。rearm()(BPM/拍子変更)は「拍0を今すぐ」を予約する
        // ため target_ms は既存の(未来の)予約より必ず小さくなり、この
        // 比較で自然に無視される。直後に続けて予約される「拍1」(新テンポ
        // での本当の次拍)は、この「拍0=今」を基準にした正しい周期になる。
        if (s_last_target_ms != 0 && target_ms > s_last_target_ms) {
            s_next_period_ms = target_ms - s_last_target_ms;
        }
        s_last_target_ms = target_ms;
    }
    portEXIT_CRITICAL(&s_mux);
}

void Midi_NotifyBeatFired(uint32_t fired_ms)
{
    (void)fired_ms; // 位相基準は「今すぐ」で十分(通知は発音直後に呼ばれる)
    bool running;
    uint32_t period;
    portENTER_CRITICAL(&s_mux);
    running = s_clock_running;
    period = s_next_period_ms;
    portEXIT_CRITICAL(&s_mux);

    if (!running || period == 0 || !s_clock_timer) return;

    uint64_t interval_us = (uint64_t)period * 1000 / kClockPpqn;
    if (interval_us < kMinClockIntervalUs) interval_us = kMinClockIntervalUs;

    esp_timer_stop(s_clock_timer); // 未アームなら INVALID_STATE(無視して良い)
    esp_timer_start_periodic(s_clock_timer, interval_us);
}

void Midi_Reset()
{
    portENTER_CRITICAL(&s_mux);
    s_clock_running = false;
    s_last_target_ms = 0;
    s_next_period_ms = 0;
    portEXIT_CRITICAL(&s_mux);
    clock_timer_stop();
}

} // namespace midi
