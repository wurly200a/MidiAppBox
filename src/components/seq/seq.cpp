// L0/L1 の実機アダプタ。ロジック本体は shared/seq_core.c(両ホスト共通)。
#include "seq.hpp"

#include "clock_authority.hpp"
#include "midi.hpp"
#include "seq_core.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace seq {
namespace {

constexpr const char* TAG = "SEQ";

// L0/L1 専用の spinlock(§6)。LVGL / ファイルシステム / オーディオ書き込みの
// ロックとは一切共有しない。臨界区間はキューとタイムラインの更新のみで、
// その中でポート送出・esp_timer 操作・ログ出力は行わない(seq_core が保証)。
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_timer_handle_t s_timer = nullptr;
ClickHandler s_click_handler = nullptr;

int64_t hook_now_us() { return clockauth::NowUs(); }
void hook_lock() { portENTER_CRITICAL(&s_mux); }
void hook_unlock() { portEXIT_CRITICAL(&s_mux); }

void hook_arm(int64_t delay_us)
{
    if (!s_timer) return;
    if (delay_us < 0) delay_us = 0;
    esp_timer_stop(s_timer); // 未アームなら INVALID_STATE(無視してよい)
    esp_timer_start_once(s_timer, (uint64_t)delay_us);
}

void hook_disarm()
{
    if (s_timer) esp_timer_stop(s_timer);
}

void hook_send_midi(const uint8_t* bytes, size_t len)
{
    midi::Midi_TxBytes(bytes, len);
}

void hook_click(uint32_t slot)
{
    if (s_click_handler) s_click_handler(slot);
}

const seqcore_hooks_t kHooks = {
    hook_now_us, hook_lock, hook_unlock, hook_arm,
    hook_disarm, hook_send_midi, hook_click,
};

void dispatch_cb(void*) { seqcore_dispatch(); }

} // namespace

void SetClickHandler(ClickHandler fn) { s_click_handler = fn; }

void Init()
{
    if (!s_timer) {
        esp_timer_create_args_t args = {};
        args.callback = dispatch_cb;
        args.name = "l0_disp";
        args.dispatch_method = ESP_TIMER_TASK;
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    }
    seqcore_init(&kHooks);
    ESP_LOGI(TAG, "L0/L1 ready (queue %d ev = %u B, tempo %d, meter %d)",
             SEQCORE_QUEUE_DEPTH,
             (unsigned)(SEQCORE_QUEUE_DEPTH * sizeof(hostapi_seq_event_t)),
             SEQCORE_TEMPO_MAX, SEQCORE_METER_MAX);
}

void Reset() { seqcore_reset(); }

#ifdef PHASE11_L0_SELFTEST
void SelfTest()
{
    const int fails = seqcore_selftest();
    ESP_LOGI(TAG, "SELFTEST %s (%d failures)", fails == 0 ? "PASS" : "FAIL", fails);
}
#endif

} // namespace seq
