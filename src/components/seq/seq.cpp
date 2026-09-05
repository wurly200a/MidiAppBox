// L0 / L1 実装(実機)。Phase 11 ステップ 1。設計は docs/architecture.md §4〜§7。
#include "seq.hpp"

#include "clock_authority.hpp"
#include "midi.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cstring>

namespace seq {
namespace {

constexpr const char* TAG = "SEQ";

// ---- 静的確保(§9: 恒久物は internal RAM の静的 BSS。ヒープから取ると
// largest free block を食い潰して WASM の linear memory 確保を壊す)----
constexpr int kQueueDepth = 256;              // 4KB(P10-4 / 9c の根拠)
constexpr int kTempoMax = 32;
constexpr int kMeterMax = 32;

constexpr uint32_t kDefaultUpq = 500000;      // 120bpm
constexpr uint32_t kUpqMin = 20000;
constexpr uint32_t kUpqMax = 10000000;

// 発火偏差の系統分(P10-3 実測 16〜26µs)。予定時刻をこの分だけ前倒しする。
constexpr int64_t kFireAdvanceUs = 20;
// これ以内の期限は arm し直さずその場で処理する(タイマ churn 回避)。
constexpr int64_t kSlackUs = 100;

l0_event_t s_queue[kQueueDepth];
int s_count = 0;

struct TempoEntry { uint32_t at_tick; uint32_t upq; };
struct MeterEntry { uint32_t at_tick; uint16_t numer; uint16_t denom; };
TempoEntry s_tempo[kTempoMax];
int s_tempo_n = 0;
MeterEntry s_meter[kMeterMax];
int s_meter_n = 0;

// L0/L1 専用の spinlock(§6)。LVGL / FS / オーディオ書き込みとは共有しない。
// 臨界区間はキューとタイムラインの更新のみ。ポート送出と esp_timer 操作は
// 必ずこの外で行う。
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t s_state = kStopped;

// 現在のテンポ区間(この区間内ではテンポ一定・ループ巻き戻しなし)。
// host_us(pb) = s_seg_us + (pb - s_seg_tick) * s_seg_upq / kPpqn
// song(pb)    = s_seg_song + (pb - s_seg_tick)
uint32_t s_seg_tick = 0;      // 区間開始の playback tick
uint32_t s_seg_song = 0;      // 区間開始の song tick
int64_t  s_seg_us = 0;        // 区間開始の host µs
uint32_t s_seg_upq = kDefaultUpq;
uint32_t s_seg_end_pb = UINT32_MAX; // 次の境界(テンポ変更 / ループ終端)

uint32_t s_next_clock_pb = 0; // 次に 0xF8 を出す playback tick(40 tick グリッド)

uint32_t s_loop_start = 0;
uint32_t s_loop_end = 0;      // end <= start でループ無効

// STOPPED 中の位置(locate / stop で更新。continue の開始点)
uint32_t s_pb_at_stop = 0;
uint32_t s_song_at_stop = 0;

esp_timer_handle_t s_timer = nullptr;
ClickHandler s_click_handler = nullptr;

// ---- テンポ / 拍子マップ(すべて s_mux 下で呼ぶ)----

uint32_t tempo_at_locked(uint32_t song_tick)
{
    uint32_t upq = kDefaultUpq;
    for (int i = 0; i < s_tempo_n; ++i) {
        if (s_tempo[i].at_tick > song_tick) break;
        upq = s_tempo[i].upq;
    }
    return upq;
}

bool next_tempo_boundary_locked(uint32_t song_tick, uint32_t* out)
{
    for (int i = 0; i < s_tempo_n; ++i) {
        if (s_tempo[i].at_tick > song_tick) { *out = s_tempo[i].at_tick; return true; }
    }
    return false;
}

// 区間の終端(playback tick)を再計算する
void seg_recompute_end_locked()
{
    uint32_t end_song = UINT32_MAX;
    uint32_t t = 0;
    if (next_tempo_boundary_locked(s_seg_song, &t)) end_song = t;
    if (s_loop_end > s_loop_start && s_loop_end > s_seg_song && s_loop_end < end_song) {
        end_song = s_loop_end;
    }
    s_seg_end_pb = (end_song == UINT32_MAX)
                       ? UINT32_MAX
                       : s_seg_tick + (end_song - s_seg_song);
}

int64_t host_us_of_locked(uint32_t pb)
{
    const int64_t d = (int64_t)pb - (int64_t)s_seg_tick;
    return s_seg_us + d * (int64_t)s_seg_upq / (int64_t)kPpqn;
}

uint32_t cur_pb_locked(int64_t now_us)
{
    const int64_t d = now_us - s_seg_us;
    if (d <= 0) return s_seg_tick;
    return s_seg_tick + (uint32_t)((d * (int64_t)kPpqn) / (int64_t)s_seg_upq);
}

// 区間境界に到達したので次の区間へ進む(playback tick は単調増加のまま、
// ループ時のみ song tick が巻き戻る。§11-1)
void seg_advance_locked()
{
    const uint32_t pb = s_seg_end_pb;
    const uint32_t song = s_seg_song + (pb - s_seg_tick);
    const int64_t us = host_us_of_locked(pb);
    s_seg_tick = pb;
    s_seg_us = us;
    s_seg_song = (s_loop_end > s_loop_start && song == s_loop_end) ? s_loop_start : song;
    s_seg_upq = tempo_at_locked(s_seg_song);
    seg_recompute_end_locked();
}

// 次に期限が来る playback tick(キュー先頭 / クロックグリッド / 区間境界の最小)
uint32_t next_deadline_locked(bool* has)
{
    uint32_t next = UINT32_MAX;
    if (s_count > 0 && s_queue[0].tick < next) next = s_queue[0].tick;
    if (s_next_clock_pb < next) next = s_next_clock_pb;
    if (s_seg_end_pb < next) next = s_seg_end_pb;
    *has = (next != UINT32_MAX);
    return next;
}

void bar_beat_locked(uint32_t song_tick, uint32_t* bar, uint16_t* beat, uint16_t* tick_in_beat)
{
    uint32_t bars = 0;
    uint32_t cur = 0;
    uint16_t numer = 4, denom = 4;
    int i = 0;
    for (;;) {
        while (i < s_meter_n && s_meter[i].at_tick <= cur) {
            numer = s_meter[i].numer;
            denom = s_meter[i].denom;
            ++i;
        }
        const uint32_t next_change = (i < s_meter_n) ? s_meter[i].at_tick : UINT32_MAX;
        const uint32_t beat_ticks = kPpqn * 4 / (denom ? denom : 4);
        const uint32_t bar_ticks = beat_ticks * (numer ? numer : 4);
        const uint32_t span_end = (next_change < song_tick) ? next_change : song_tick;
        const uint32_t span = span_end - cur;
        if (span_end == song_tick) {
            bars += span / bar_ticks;
            const uint32_t r = span % bar_ticks;
            *bar = bars;
            *beat = (uint16_t)(r / beat_ticks);
            *tick_in_beat = (uint16_t)(r % beat_ticks);
            return;
        }
        // 拍子変更点。小節途中で変わった場合は新しい小節が始まるものとして扱う
        bars += (span + bar_ticks - 1) / bar_ticks;
        cur = span_end;
    }
}

// ---- ポート層(§7)。必ずロックの外から呼ぶ ----

size_t midi_msg_len(uint8_t status)
{
    if (status >= 0xF8) return 1;                 // System Realtime
    if (status >= 0xF0) {
        switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;
        }
    }
    const uint8_t hi = status & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
}

void port_send_realtime(uint8_t b)
{
    midi::Midi_TxBytes(&b, 1);
}

void port_dispatch(const l0_event_t& ev)
{
    switch (ev.port) {
    case kPortDinOut: {
        if (ev.status < 0x80) return;             // オペコードは DIN_OUT では無意味
        uint8_t buf[3];
        const size_t len = midi_msg_len(ev.status);
        buf[0] = ev.status;
        buf[1] = ev.data1;
        buf[2] = ev.data2;
        // §7: 小さなメッセージ(3〜4 バイト)は一括書き込みでよい。
        // 大きなバーストは分割する規律だが、L0 のイベントは最大 3 バイト。
        midi::Midi_TxBytes(buf, len);
        break;
    }
    case kPortClick:
        if (ev.status == kOpTone && s_click_handler) s_click_handler(ev.param);
        break;
    default:
        break;                                    // USB_MIDI / SYNTH は予約のみ
    }
}

void timer_arm(int64_t delay_us)
{
    if (!s_timer) return;
    if (delay_us < 0) delay_us = 0;
    esp_timer_stop(s_timer); // 未アームなら INVALID_STATE(無視してよい)
    esp_timer_start_once(s_timer, (uint64_t)delay_us);
}

// アプリタスク側の変更(seq_write / tempo 変更 / locate 等)の後に呼ぶ。
// **イベントの取り出しは行わない**(取り出しと送出は esp_timer タスクだけの
// 責務にして、2 タスクからの二重発火・順序逆転を構造的に排除する)。
void rearm()
{
    int64_t arm = -1;
    portENTER_CRITICAL(&s_mux);
    if (s_state == kPlaying) {
        bool has = false;
        const uint32_t next = next_deadline_locked(&has);
        if (has) {
            const int64_t due = host_us_of_locked(next) - kFireAdvanceUs;
            arm = due - clockauth::NowUs();
            if (arm < 0) arm = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (arm >= 0) timer_arm(arm);
}

// L0 ディスパッチャ本体(esp_timer タスク上)。
// 再アームは常に「予定 tick → host µs」の絶対時刻グリッド基準であり、
// 発火時刻からの相対加算は行わない(§6)。
void dispatch_cb(void*)
{
    constexpr int kMaxIter = 2048;
    for (int iter = 0; iter < kMaxIter; ++iter) {
        l0_event_t emit[16];
        int nemit = 0;
        bool emit_clock = false;
        int64_t arm = -1;

        portENTER_CRITICAL(&s_mux);
        if (s_state != kPlaying) { portEXIT_CRITICAL(&s_mux); return; }
        bool has = false;
        const uint32_t next = next_deadline_locked(&has);
        if (!has) { portEXIT_CRITICAL(&s_mux); return; }
        const int64_t due = host_us_of_locked(next) - kFireAdvanceUs;
        const int64_t now = clockauth::NowUs();
        if (due > now + kSlackUs) {
            arm = due - now;
            portEXIT_CRITICAL(&s_mux);
            timer_arm(arm);
            return;
        }
        if (next == s_seg_end_pb) seg_advance_locked();
        if (next == s_next_clock_pb) {
            emit_clock = true;
            s_next_clock_pb += kClockGridTicks;
        }
        while (s_count > 0 && s_queue[0].tick <= next && nemit < (int)(sizeof(emit) / sizeof(emit[0]))) {
            emit[nemit++] = s_queue[0];
            if (s_count > 1) memmove(&s_queue[0], &s_queue[1], (size_t)(s_count - 1) * sizeof(l0_event_t));
            s_count--;
        }
        portEXIT_CRITICAL(&s_mux);

        // 送出はロックの外。リアルタイムバイトを最優先で出す(§7-4)
        if (emit_clock) port_send_realtime(0xF8);
        for (int i = 0; i < nemit; ++i) port_dispatch(emit[i]);
    }
    // 反復上限に達した(過去 tick の大量流し込み等)。続きは次の起動で。
    timer_arm(0);
}

void timer_ensure()
{
    if (s_timer) return;
    esp_timer_create_args_t args = {};
    args.callback = dispatch_cb;
    args.name = "l0_disp";
    args.dispatch_method = ESP_TIMER_TASK;
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
}

void reset_state_locked()
{
    s_count = 0;
    s_tempo_n = 0;
    s_meter_n = 0;
    s_state = kStopped;
    s_seg_tick = 0;
    s_seg_song = 0;
    s_seg_us = 0;
    s_seg_upq = kDefaultUpq;
    s_seg_end_pb = UINT32_MAX;
    s_next_clock_pb = 0;
    s_loop_start = 0;
    s_loop_end = 0;
    s_pb_at_stop = 0;
    s_song_at_stop = 0;
}

} // namespace

void SetClickHandler(ClickHandler fn) { s_click_handler = fn; }

void Init()
{
    timer_ensure();
    portENTER_CRITICAL(&s_mux);
    reset_state_locked();
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "L0/L1 ready (queue %d ev = %u B, tempo %d, meter %d)",
             kQueueDepth, (unsigned)sizeof(s_queue), kTempoMax, kMeterMax);
}

void Reset()
{
    bool was_playing;
    portENTER_CRITICAL(&s_mux);
    was_playing = (s_state == kPlaying);
    reset_state_locked();
    portEXIT_CRITICAL(&s_mux);
    if (s_timer) esp_timer_stop(s_timer);
    if (was_playing) port_send_realtime(0xFC); // 再生中のアプリ破棄は Stop を出す
}

// ---- transport ----

int32_t TransportStart()
{
    portENTER_CRITICAL(&s_mux);
    if (s_state == kPlaying) { portEXIT_CRITICAL(&s_mux); return -1; }
    s_count = 0;
    s_seg_tick = 0;
    s_seg_song = 0;
    s_seg_us = clockauth::NowUs();
    s_seg_upq = tempo_at_locked(0);
    s_next_clock_pb = 0;
    seg_recompute_end_locked();
    s_state = kPlaying;
    portEXIT_CRITICAL(&s_mux);

    port_send_realtime(0xFA);
    rearm();
    return 0;
}

int32_t TransportStop()
{
    portENTER_CRITICAL(&s_mux);
    if (s_state != kPlaying) { portEXIT_CRITICAL(&s_mux); return -1; }
    const uint32_t pb = cur_pb_locked(clockauth::NowUs());
    s_pb_at_stop = pb;
    s_song_at_stop = s_seg_song + (pb - s_seg_tick);
    s_state = kStopped;
    s_count = 0; // 未発火イベントは破棄する(All Notes Off はアプリ責務。§11-8)
    portEXIT_CRITICAL(&s_mux);

    if (s_timer) esp_timer_stop(s_timer);
    port_send_realtime(0xFC);
    return 0;
}

int32_t TransportContinue()
{
    portENTER_CRITICAL(&s_mux);
    if (s_state == kPlaying) { portEXIT_CRITICAL(&s_mux); return -1; }
    s_seg_tick = s_pb_at_stop;
    s_seg_song = s_song_at_stop;
    s_seg_us = clockauth::NowUs();
    s_seg_upq = tempo_at_locked(s_seg_song);
    // クロックグリッドは playback tick の 40 tick 格子上で連続させる
    s_next_clock_pb = ((s_seg_tick + kClockGridTicks - 1) / kClockGridTicks) * kClockGridTicks;
    seg_recompute_end_locked();
    s_state = kPlaying;
    portEXIT_CRITICAL(&s_mux);

    port_send_realtime(0xFB);
    rearm();
    return 0;
}

int32_t TransportLocate(uint32_t song_tick)
{
    portENTER_CRITICAL(&s_mux);
    if (s_state != kPlaying) {
        s_song_at_stop = song_tick; // 次の continue の開始位置
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }
    const int64_t now = clockauth::NowUs();
    const uint32_t pb = cur_pb_locked(now);
    s_seg_tick = pb;                    // playback tick は単調増加のまま
    s_seg_song = song_tick;
    s_seg_us = now;
    s_seg_upq = tempo_at_locked(song_tick);
    s_count = 0;                        // 移動前の位置に対する予約は破棄
    seg_recompute_end_locked();
    portEXIT_CRITICAL(&s_mux);

    rearm();
    return 0;
}

int32_t TransportGetPosition(void* buf, size_t buf_len)
{
    if (buf == nullptr || buf_len < sizeof(l1_position_t)) return -1;
    l1_position_t pos{};

    portENTER_CRITICAL(&s_mux);
    const int64_t now = clockauth::NowUs();
    uint32_t pb, song;
    if (s_state == kPlaying) {
        pb = cur_pb_locked(now);
        song = s_seg_song + (pb - s_seg_tick);
    } else {
        pb = s_pb_at_stop;
        song = s_song_at_stop;
    }
    pos.host_us = (uint64_t)now;
    pos.tick = pb;
    pos.song_tick = song;
    pos.tempo_upq = (s_state == kPlaying) ? s_seg_upq : tempo_at_locked(song);
    pos.state = s_state;
    bar_beat_locked(song, &pos.bar, &pos.beat, &pos.tick_in_beat);
    portEXIT_CRITICAL(&s_mux);

    memcpy(buf, &pos, sizeof(pos));
    return 0;
}

// ---- tempomap ----

int32_t TempoMapSetTempo(uint32_t at_song_tick, uint32_t us_per_quarter)
{
    if (us_per_quarter < kUpqMin || us_per_quarter > kUpqMax) return -1;

    bool need_reanchor = false;
    portENTER_CRITICAL(&s_mux);
    if (s_state == kPlaying) {
        const uint32_t cur_song = s_seg_song + (cur_pb_locked(clockauth::NowUs()) - s_seg_tick);
        if (at_song_tick < cur_song) { portEXIT_CRITICAL(&s_mux); return -1; }
        need_reanchor = (at_song_tick == cur_song);
    }
    // 挿入(同一 at_tick は上書き)。at_tick 昇順を保つ
    int i = 0;
    while (i < s_tempo_n && s_tempo[i].at_tick < at_song_tick) ++i;
    if (i < s_tempo_n && s_tempo[i].at_tick == at_song_tick) {
        s_tempo[i].upq = us_per_quarter;
    } else {
        if (s_tempo_n >= kTempoMax) { portEXIT_CRITICAL(&s_mux); return -1; }
        if (i < s_tempo_n) {
            memmove(&s_tempo[i + 1], &s_tempo[i], (size_t)(s_tempo_n - i) * sizeof(TempoEntry));
        }
        s_tempo[i].at_tick = at_song_tick;
        s_tempo[i].upq = us_per_quarter;
        s_tempo_n++;
    }
    if (s_state == kPlaying) {
        if (need_reanchor) {
            const int64_t now = clockauth::NowUs();
            const uint32_t pb = cur_pb_locked(now);
            s_seg_song = s_seg_song + (pb - s_seg_tick);
            s_seg_tick = pb;
            s_seg_us = now;
            s_seg_upq = us_per_quarter;
        }
        // キューの積み直しは不要(ソートキーが tick なので変換係数だけが変わる。§6)
        seg_recompute_end_locked();
    }
    portEXIT_CRITICAL(&s_mux);

    rearm();
    return 0;
}

int32_t TempoMapSetMeter(uint32_t at_song_tick, uint32_t numer, uint32_t denom)
{
    if (numer < 1 || numer > 32) return -1;
    if (denom != 1 && denom != 2 && denom != 4 && denom != 8 && denom != 16) return -1;

    portENTER_CRITICAL(&s_mux);
    int i = 0;
    while (i < s_meter_n && s_meter[i].at_tick < at_song_tick) ++i;
    if (i < s_meter_n && s_meter[i].at_tick == at_song_tick) {
        s_meter[i].numer = (uint16_t)numer;
        s_meter[i].denom = (uint16_t)denom;
    } else {
        if (s_meter_n >= kMeterMax) { portEXIT_CRITICAL(&s_mux); return -1; }
        if (i < s_meter_n) {
            memmove(&s_meter[i + 1], &s_meter[i], (size_t)(s_meter_n - i) * sizeof(MeterEntry));
        }
        s_meter[i].at_tick = at_song_tick;
        s_meter[i].numer = (uint16_t)numer;
        s_meter[i].denom = (uint16_t)denom;
        s_meter_n++;
    }
    portEXIT_CRITICAL(&s_mux);
    return 0;
}

int32_t TempoMapSetLoop(uint32_t start_song_tick, uint32_t end_song_tick)
{
    if (start_song_tick == 0 && end_song_tick == 0) {
        portENTER_CRITICAL(&s_mux);
        s_loop_start = 0;
        s_loop_end = 0;
        if (s_state == kPlaying) seg_recompute_end_locked();
        portEXIT_CRITICAL(&s_mux);
        rearm();
        return 0;
    }
    if (start_song_tick >= end_song_tick) return -1;

    portENTER_CRITICAL(&s_mux);
    s_loop_start = start_song_tick;
    s_loop_end = end_song_tick;
    if (s_state == kPlaying) seg_recompute_end_locked();
    portEXIT_CRITICAL(&s_mux);
    rearm();
    return 0;
}

// ---- seq ----

int32_t SeqWrite(const void* buf, size_t buf_len)
{
    if (buf == nullptr) return 0;
    const size_t want = buf_len / sizeof(l0_event_t);
    if (want == 0) return 0;
    const l0_event_t* src = static_cast<const l0_event_t*>(buf);

    int32_t n = 0;
    portENTER_CRITICAL(&s_mux);
    while ((size_t)n < want && s_count < kQueueDepth) {
        const l0_event_t ev = src[n];
        // 安定な挿入ソート: 同一 tick の後ろへ入れる(書き込み順を保つ)
        int i = s_count;
        while (i > 0 && s_queue[i - 1].tick > ev.tick) --i;
        if (i < s_count) {
            memmove(&s_queue[i + 1], &s_queue[i], (size_t)(s_count - i) * sizeof(l0_event_t));
        }
        s_queue[i] = ev;
        s_count++;
        n++;
    }
    portEXIT_CRITICAL(&s_mux);

    if (n > 0) rearm();
    return n; // プレフィックス受理(残りはアプリが再送する。§11-9)
}

int32_t SeqFlushAfter(uint32_t tick)
{
    int32_t removed = 0;
    portENTER_CRITICAL(&s_mux);
    int keep = 0;
    for (int i = 0; i < s_count; ++i) {
        if (s_queue[i].tick >= tick) { removed++; continue; }
        if (keep != i) s_queue[keep] = s_queue[i];
        keep++;
    }
    s_count = keep;
    portEXIT_CRITICAL(&s_mux);

    if (removed > 0) rearm();
    return removed;
}

int32_t SeqFilledUntil()
{
    portENTER_CRITICAL(&s_mux);
    uint32_t t;
    if (s_count > 0) {
        t = s_queue[s_count - 1].tick;
    } else if (s_state == kPlaying) {
        t = cur_pb_locked(clockauth::NowUs());
    } else {
        t = s_pb_at_stop;
    }
    portEXIT_CRITICAL(&s_mux);
    return (int32_t)t;
}

int32_t TimeUsToTick(int64_t us)
{
    portENTER_CRITICAL(&s_mux);
    if (s_state != kPlaying) { portEXIT_CRITICAL(&s_mux); return -1; }
    const int64_t d = us - s_seg_us;
    int64_t tick = (int64_t)s_seg_tick + (d * (int64_t)kPpqn) / (int64_t)s_seg_upq;
    portEXIT_CRITICAL(&s_mux);
    if (tick < 0) tick = 0;
    return (int32_t)tick;
}

#ifdef PHASE11_L0_SELFTEST
void SelfTest()
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { fails++; ESP_LOGE(TAG, "SELFTEST FAIL: %s", what); }
    };

    Reset();

    // 1. tick 昇順に整列すること(降順で投入する)
    l0_event_t evs[5] = {};
    for (int i = 0; i < 5; ++i) {
        evs[i].tick = (uint32_t)(4 - i) * 100;
        evs[i].port = kPortClick;
        evs[i].status = kOpTone;
        evs[i].data1 = (uint8_t)i;
    }
    check(SeqWrite(evs, sizeof(evs)) == 5, "write 5");
    for (int i = 1; i < s_count; ++i) check(s_queue[i - 1].tick <= s_queue[i].tick, "ascending");
    check(s_queue[0].tick == 0 && s_queue[4].tick == 400, "sorted range");

    // 2. 同一 tick は書き込み順を保つこと
    Reset();
    l0_event_t same[4] = {};
    for (int i = 0; i < 4; ++i) {
        same[i].tick = 960;
        same[i].port = kPortClick;
        same[i].status = kOpTone;
        same[i].data1 = (uint8_t)(10 + i);
    }
    check(SeqWrite(same, sizeof(same)) == 4, "write same tick");
    for (int i = 0; i < 4; ++i) check(s_queue[i].data1 == (uint8_t)(10 + i), "stable order");

    // 3. 満杯時の受理数(プレフィックス受理)
    Reset();
    static l0_event_t bulk[kQueueDepth];
    for (int i = 0; i < kQueueDepth; ++i) { bulk[i] = l0_event_t{}; bulk[i].tick = (uint32_t)i; }
    check(SeqWrite(bulk, sizeof(bulk)) == kQueueDepth, "fill to depth");
    check(SeqWrite(bulk, sizeof(l0_event_t) * 10) == 0, "full -> 0");
    Reset();
    check(SeqWrite(bulk, sizeof(l0_event_t) * (kQueueDepth - 4)) == kQueueDepth - 4, "fill 252");
    check(SeqWrite(bulk, sizeof(l0_event_t) * 10) == 4, "partial accept 4");

    // 4. flush_after の件数
    Reset();
    check(SeqWrite(bulk, sizeof(l0_event_t) * 100) == 100, "write 100");
    check(SeqFlushAfter(60) == 40, "flush_after(60) removes 40");
    check(SeqFilledUntil() == 59, "filled_until after flush");
    check(SeqFlushAfter(0) == 60, "flush_after(0) removes all");

    // 5. 端数バイトは無視される
    Reset();
    check(SeqWrite(bulk, sizeof(l0_event_t) * 2 + 7) == 2, "odd bytes ignored");

    Reset();
    ESP_LOGI(TAG, "SELFTEST %s (%d failures)", fails == 0 ? "PASS" : "FAIL", fails);
}
#endif

} // namespace seq
