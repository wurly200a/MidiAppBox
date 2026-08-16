// Phase 8b 追記(2026-08-15): QY70 切り分け用の一時検証コード(使い捨て)。
// docs/prompts/phase08b_midi_clock_api.md の「追記」節参照。
// Clock (0xF8) も Start/Stop も一切送らず、Note On/Off だけを送り続ける
// (Phase 8a の疎通確認と同じ最小構成)。既存の hostapi_midi_send の実体
// (midi::Midi_Send)を直接呼ぶだけで、Host API/ABI は変更しない。

#include "phase08b_qy70_debug.hpp"

#include "midi.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

void qy70_debug_task(void*)
{
    // QY70 の受信チャンネル設定・MIDI ケーブル接続をユーザーが整える時間
    vTaskDelay(pdMS_TO_TICKS(5000));

    const uint8_t note_on[3]  = {0x90, 60, 100}; // ch1, note=60, vel=100
    const uint8_t note_off[3] = {0x80, 60, 0};

    while (true) {
        midi::Midi_Send(note_on, sizeof(note_on));
        vTaskDelay(pdMS_TO_TICKS(300));
        midi::Midi_Send(note_off, sizeof(note_off));
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

} // namespace

void phase08b_qy70_debug_start()
{
    xTaskCreate(qy70_debug_task, "qy70_dbg", 3072, nullptr, 4, nullptr);
}
