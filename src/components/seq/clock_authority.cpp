// Clock Authority 実装(実機)。詳細は clock_authority.hpp / docs/architecture.md §3。
#include "clock_authority.hpp"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace clockauth {
namespace {

// ISR(on_sent)と通常タスクの両方から触るので専用の spinlock。
// L0 のキューロックとも LVGL/FS/オーディオ書き込みとも共有しない(§6)。
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t s_rate_hz = 44100;
uint8_t  s_bytes_per_frame = 4; // 16bit stereo

uint64_t s_frames = 0;          // 累計フレーム(再構成をまたいで単調増加)
bool     s_anchored = false;
uint64_t s_anchor_frames = 0;
int64_t  s_anchor_us = 0;

// 実機は I2S と esp_timer が同一水晶(P10-2: -0.00ppm)なので固定オフセット 0。
// 別クロック系のホストへ移植するときはここに比の補正が入る。
constexpr int64_t kOffsetUs = 0;

} // namespace

void Init()
{
    portENTER_CRITICAL(&s_mux);
    s_frames = 0;
    s_anchored = false;
    s_anchor_frames = 0;
    s_anchor_us = 0;
    portEXIT_CRITICAL(&s_mux);
}

void OnSent(size_t bytes)
{
    // ISR コンテキスト。整数演算のみ。
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_mux);
    s_frames += (uint64_t)bytes / (s_bytes_per_frame ? s_bytes_per_frame : 4);
    if (!s_anchored) {
        s_anchored = true;
        s_anchor_frames = s_frames;
        s_anchor_us = now;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

void OnFormatChanged(uint32_t rate_hz, uint8_t bits, bool stereo)
{
    const uint8_t bpf = (uint8_t)((bits >= 32 ? 4 : 2) * (stereo ? 2 : 1));
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_rate_hz = rate_hz ? rate_hz : 44100;
    s_bytes_per_frame = bpf ? bpf : 4;
    // 再構成中は on_sent が止まる。アンカーを張り替えて、以後の ppm 推定が
    // 停止区間を誤差として拾わないようにする(音楽時間軸は esp_timer 外挿で
    // 連続しているので、ここで時刻が飛ぶことはない)。
    s_anchor_frames = s_frames;
    s_anchor_us = now;
    s_anchored = true;
    portEXIT_CRITICAL(&s_mux);
}

int64_t NowUs()
{
    return esp_timer_get_time() + kOffsetUs;
}

int64_t HostUsFromTimerUs(int64_t timer_us)
{
    return timer_us + kOffsetUs;
}

uint64_t Frames()
{
    portENTER_CRITICAL(&s_mux);
    const uint64_t f = s_frames;
    portEXIT_CRITICAL(&s_mux);
    return f;
}

int32_t EstimatedPpm()
{
    portENTER_CRITICAL(&s_mux);
    const bool anchored = s_anchored;
    const uint64_t df = s_frames - s_anchor_frames;
    const int64_t anchor_us = s_anchor_us;
    const uint32_t rate = s_rate_hz;
    portEXIT_CRITICAL(&s_mux);

    if (!anchored) return 0;
    const int64_t elapsed_us = esp_timer_get_time() - anchor_us;
    if (elapsed_us < 1000000) return 0; // 1 秒未満は分解能不足
    // 実効レート = df / elapsed。公称比の偏差を ppm で返す。
    // (df * 1e6 / elapsed) / rate - 1 を 1e6 倍する = df*1e12/(elapsed*rate) - 1e6
    const int64_t num = (int64_t)df * 1000000LL;      // frames * 1e6
    const int64_t den = elapsed_us;                    // µs
    // effective_rate_milli = num*1000/den(ミリ Hz)
    const int64_t eff_milli = num * 1000LL / (den ? den : 1);
    const int64_t nom_milli = (int64_t)rate * 1000LL;
    if (nom_milli == 0) return 0;
    return (int32_t)((eff_milli - nom_milli) * 1000000LL / nom_milli);
}

} // namespace clockauth
