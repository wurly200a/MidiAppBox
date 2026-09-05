/* L0 / L1 コア実装(移植可能な C)。詳細は seq_core.h / docs/architecture.md。 */
#include "seq_core.h"

#include <string.h>

/* ABI 凍結の確認。L0 のキュー要素はアプリが seq_write に渡すものと同一
 * レイアウトであり、境界での変換コストがゼロであることが前提(§6)*/
_Static_assert(sizeof(hostapi_seq_event_t) == 16, "hostapi_seq_event_t must be 16 bytes");
_Static_assert(sizeof(hostapi_position_t) == 32, "hostapi_position_t must be 32 bytes");

/* ---- 定数 ---- */
#define DEFAULT_UPQ 500000u /* 120bpm */
#define UPQ_MIN 20000u
#define UPQ_MAX 10000000u

/* 発火偏差の系統分(P10-3 実測 16〜26µs)。予定時刻をこの分だけ前倒しする */
#define FIRE_ADVANCE_US 20
/* これ以内の期限は arm し直さずその場で処理する(タイマ churn 回避) */
#define SLACK_US 100
/* 1 回のディスパッチで取り出す上限(臨界区間を短く保つ) */
#define EMIT_MAX 16
/* ディスパッチャの反復上限(過去 tick の大量流し込みでの暴走防止) */
#define ITER_MAX 2048

/* ---- 状態(すべて hooks->lock 下で触る)---- */
static const seqcore_hooks_t* s_hooks;

static hostapi_seq_event_t s_queue[SEQCORE_QUEUE_DEPTH];
static int s_count;

typedef struct { uint32_t at_tick; uint32_t upq; } TempoEntry;
typedef struct { uint32_t at_tick; uint16_t numer; uint16_t denom; } MeterEntry;
static TempoEntry s_tempo[SEQCORE_TEMPO_MAX];
static int s_tempo_n;
static MeterEntry s_meter[SEQCORE_METER_MAX];
static int s_meter_n;

static uint32_t s_state;

/* 現在のテンポ区間(この区間内ではテンポ一定・ループ巻き戻しなし)
 *   host_us(pb) = s_seg_us + (pb - s_seg_tick) * s_seg_upq / PPQN
 *   song(pb)    = s_seg_song + (pb - s_seg_tick)
 */
static uint32_t s_seg_tick;
static uint32_t s_seg_song;
static int64_t  s_seg_us;
static uint32_t s_seg_upq;
static uint32_t s_seg_end_pb; /* 次の境界(テンポ変更 / ループ終端) */

static uint32_t s_next_clock_pb; /* 次に 0xF8 を出す playback tick */

static uint32_t s_loop_start;
static uint32_t s_loop_end; /* end <= start でループ無効 */

static uint32_t s_pb_at_stop;   /* STOPPED 中の playback tick */
static uint32_t s_song_at_stop; /* STOPPED 中の song tick(continue の開始点) */

static void lock(void)   { if (s_hooks && s_hooks->lock) s_hooks->lock(); }
static void unlock(void) { if (s_hooks && s_hooks->unlock) s_hooks->unlock(); }
static int64_t now_us(void) { return (s_hooks && s_hooks->now_us) ? s_hooks->now_us() : 0; }

/* ---- テンポ / 拍子マップ(ロック下で呼ぶ)---- */

static uint32_t tempo_at_locked(uint32_t song_tick)
{
    uint32_t upq = DEFAULT_UPQ;
    for (int i = 0; i < s_tempo_n; ++i) {
        if (s_tempo[i].at_tick > song_tick) break;
        upq = s_tempo[i].upq;
    }
    return upq;
}

static bool next_tempo_boundary_locked(uint32_t song_tick, uint32_t* out)
{
    for (int i = 0; i < s_tempo_n; ++i) {
        if (s_tempo[i].at_tick > song_tick) { *out = s_tempo[i].at_tick; return true; }
    }
    return false;
}

static void seg_recompute_end_locked(void)
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

static int64_t host_us_of_locked(uint32_t pb)
{
    const int64_t d = (int64_t)pb - (int64_t)s_seg_tick;
    return s_seg_us + d * (int64_t)s_seg_upq / (int64_t)HOSTAPI_PPQN;
}

static uint32_t cur_pb_locked(int64_t t_us)
{
    const int64_t d = t_us - s_seg_us;
    if (d <= 0) return s_seg_tick;
    return s_seg_tick + (uint32_t)((d * (int64_t)HOSTAPI_PPQN) / (int64_t)s_seg_upq);
}

/* 区間境界に到達したので次の区間へ進む。playback tick は単調増加のまま、
 * ループ時のみ song tick が巻き戻る(§11-1)*/
static void seg_advance_locked(void)
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

/* 次に期限が来る playback tick(キュー先頭 / クロックグリッド / 区間境界の最小)*/
static uint32_t next_deadline_locked(bool* has)
{
    uint32_t next = UINT32_MAX;
    if (s_count > 0 && s_queue[0].tick < next) next = s_queue[0].tick;
    if (s_next_clock_pb < next) next = s_next_clock_pb;
    if (s_seg_end_pb < next) next = s_seg_end_pb;
    *has = (next != UINT32_MAX);
    return next;
}

static void bar_beat_locked(uint32_t song_tick, uint32_t* bar, uint16_t* beat,
                            uint16_t* tick_in_beat)
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
        const uint32_t beat_ticks = (uint32_t)HOSTAPI_PPQN * 4u / (denom ? denom : 4u);
        const uint32_t bar_ticks = beat_ticks * (numer ? numer : 4u);
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
        /* 拍子変更点。小節途中で変わった場合は新しい小節が始まるものとして扱う */
        bars += (span + bar_ticks - 1) / bar_ticks;
        cur = span_end;
    }
}

/* ---- ポート層(§7)。必ずロックの外から呼ぶ ---- */

static size_t midi_msg_len(uint8_t status)
{
    if (status >= 0xF8) return 1; /* System Realtime */
    if (status >= 0xF0) {
        switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;
        }
    }
    {
        const uint8_t hi = (uint8_t)(status & 0xF0);
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
}

static void port_send_realtime(uint8_t b)
{
    if (s_hooks && s_hooks->send_midi) s_hooks->send_midi(&b, 1);
}

static void port_dispatch(const hostapi_seq_event_t* ev)
{
    switch (ev->port) {
    case HOSTAPI_PORT_DIN_OUT: {
        if (ev->status < 0x80) return; /* オペコードは DIN_OUT では無意味 */
        uint8_t buf[3];
        const size_t len = midi_msg_len(ev->status);
        buf[0] = ev->status;
        buf[1] = ev->data1;
        buf[2] = ev->data2;
        /* §7: 小さなメッセージ(3〜4 バイト)は一括書き込みでよい */
        if (s_hooks && s_hooks->send_midi) s_hooks->send_midi(buf, len);
        break;
    }
    case HOSTAPI_PORT_CLICK:
        if (ev->status == HOSTAPI_SEQ_OP_TONE && s_hooks && s_hooks->click) {
            s_hooks->click(ev->param);
        }
        break;
    default:
        break; /* USB_MIDI / SYNTH は予約のみ */
    }
}

static void timer_arm(int64_t delay_us)
{
    if (delay_us < 0) delay_us = 0;
    if (s_hooks && s_hooks->arm) s_hooks->arm(delay_us);
}

static void timer_disarm(void)
{
    if (s_hooks && s_hooks->disarm) s_hooks->disarm();
}

/* アプリ側の変更(seq_write / テンポ変更 / locate 等)の後に呼ぶ。
 * **イベントの取り出しは行わない**(取り出しと送出はディスパッチャだけの
 * 責務にして、2 スレッドからの二重発火・順序逆転を構造的に排除する)*/
static void rearm(void)
{
    int64_t arm = -1;
    lock();
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) {
        bool has = false;
        const uint32_t next = next_deadline_locked(&has);
        if (has) {
            arm = host_us_of_locked(next) - FIRE_ADVANCE_US - now_us();
            if (arm < 0) arm = 0;
        }
    }
    unlock();
    if (arm >= 0) timer_arm(arm);
}

static void reset_state_locked(void)
{
    s_count = 0;
    s_tempo_n = 0;
    s_meter_n = 0;
    s_state = HOSTAPI_TRANSPORT_STOPPED;
    s_seg_tick = 0;
    s_seg_song = 0;
    s_seg_us = 0;
    s_seg_upq = DEFAULT_UPQ;
    s_seg_end_pb = UINT32_MAX;
    s_next_clock_pb = 0;
    s_loop_start = 0;
    s_loop_end = 0;
    s_pb_at_stop = 0;
    s_song_at_stop = 0;
}

/* ---- 公開 API ---- */

void seqcore_init(const seqcore_hooks_t* hooks)
{
    s_hooks = hooks;
    lock();
    reset_state_locked();
    unlock();
}

void seqcore_reset(void)
{
    bool was_playing;
    lock();
    was_playing = (s_state == HOSTAPI_TRANSPORT_PLAYING);
    reset_state_locked();
    unlock();
    timer_disarm();
    if (was_playing) port_send_realtime(0xFC); /* 再生中のアプリ破棄は Stop を出す */
}

void seqcore_dispatch(void)
{
    for (int iter = 0; iter < ITER_MAX; ++iter) {
        hostapi_seq_event_t emit[EMIT_MAX];
        int nemit = 0;
        bool emit_clock = false;
        int64_t arm;

        lock();
        if (s_state != HOSTAPI_TRANSPORT_PLAYING) { unlock(); return; }
        bool has = false;
        const uint32_t next = next_deadline_locked(&has);
        if (!has) { unlock(); return; }
        const int64_t due = host_us_of_locked(next) - FIRE_ADVANCE_US;
        const int64_t t_now = now_us();
        if (due > t_now + SLACK_US) {
            arm = due - t_now;
            unlock();
            timer_arm(arm);
            return;
        }
        if (next == s_seg_end_pb) seg_advance_locked();
        if (next == s_next_clock_pb) {
            emit_clock = true;
            s_next_clock_pb += SEQCORE_CLOCK_GRID_TICKS;
        }
        while (s_count > 0 && s_queue[0].tick <= next && nemit < EMIT_MAX) {
            emit[nemit++] = s_queue[0];
            if (s_count > 1) {
                memmove(&s_queue[0], &s_queue[1],
                        (size_t)(s_count - 1) * sizeof(hostapi_seq_event_t));
            }
            s_count--;
        }
        unlock();

        /* 送出はロックの外。リアルタイムバイトを最優先で出す(§7-4)*/
        if (emit_clock) port_send_realtime(0xF8);
        for (int i = 0; i < nemit; ++i) port_dispatch(&emit[i]);
    }
    /* 反復上限に達した。続きは次の起動で */
    timer_arm(0);
}

int32_t seqcore_transport_start(void)
{
    lock();
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) { unlock(); return -1; }
    s_count = 0;
    s_seg_tick = 0;
    s_seg_song = 0;
    s_seg_us = now_us();
    s_seg_upq = tempo_at_locked(0);
    s_next_clock_pb = 0;
    seg_recompute_end_locked();
    s_state = HOSTAPI_TRANSPORT_PLAYING;
    unlock();

    port_send_realtime(0xFA);
    rearm();
    return 0;
}

int32_t seqcore_transport_stop(void)
{
    lock();
    if (s_state != HOSTAPI_TRANSPORT_PLAYING) { unlock(); return -1; }
    {
        const uint32_t pb = cur_pb_locked(now_us());
        s_pb_at_stop = pb;
        s_song_at_stop = s_seg_song + (pb - s_seg_tick);
    }
    s_state = HOSTAPI_TRANSPORT_STOPPED;
    s_count = 0; /* 未発火イベントは破棄(All Notes Off はアプリ責務。§11-8)*/
    unlock();

    timer_disarm();
    port_send_realtime(0xFC);
    return 0;
}

int32_t seqcore_transport_continue(void)
{
    lock();
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) { unlock(); return -1; }
    s_seg_tick = s_pb_at_stop;
    s_seg_song = s_song_at_stop;
    s_seg_us = now_us();
    s_seg_upq = tempo_at_locked(s_seg_song);
    /* クロックグリッドは playback tick の 40 tick 格子上で連続させる */
    s_next_clock_pb = ((s_seg_tick + SEQCORE_CLOCK_GRID_TICKS - 1) /
                       SEQCORE_CLOCK_GRID_TICKS) * SEQCORE_CLOCK_GRID_TICKS;
    seg_recompute_end_locked();
    s_state = HOSTAPI_TRANSPORT_PLAYING;
    unlock();

    port_send_realtime(0xFB);
    rearm();
    return 0;
}

int32_t seqcore_transport_locate(uint32_t song_tick)
{
    lock();
    if (s_state != HOSTAPI_TRANSPORT_PLAYING) {
        s_song_at_stop = song_tick; /* 次の continue の開始位置 */
        unlock();
        return 0;
    }
    {
        const int64_t t = now_us();
        const uint32_t pb = cur_pb_locked(t);
        s_seg_tick = pb; /* playback tick は単調増加のまま */
        s_seg_song = song_tick;
        s_seg_us = t;
        s_seg_upq = tempo_at_locked(song_tick);
        s_count = 0; /* 移動前の位置に対する予約は破棄 */
        seg_recompute_end_locked();
    }
    unlock();

    rearm();
    return 0;
}

int32_t seqcore_transport_get_position(void* buf, size_t buf_len)
{
    hostapi_position_t pos;
    if (buf == NULL || buf_len < sizeof(hostapi_position_t)) return -1;
    memset(&pos, 0, sizeof(pos));

    lock();
    {
        const int64_t t = now_us();
        uint32_t pb, song;
        if (s_state == HOSTAPI_TRANSPORT_PLAYING) {
            pb = cur_pb_locked(t);
            song = s_seg_song + (pb - s_seg_tick);
        } else {
            pb = s_pb_at_stop;
            song = s_song_at_stop;
        }
        pos.host_us = (uint64_t)t;
        pos.tick = pb;
        pos.song_tick = song;
        pos.tempo_upq = (s_state == HOSTAPI_TRANSPORT_PLAYING) ? s_seg_upq : tempo_at_locked(song);
        pos.state = s_state;
        bar_beat_locked(song, &pos.bar, &pos.beat, &pos.tick_in_beat);
    }
    unlock();

    memcpy(buf, &pos, sizeof(pos));
    return 0;
}

int32_t seqcore_tempomap_set_tempo(uint32_t at_song_tick, uint32_t us_per_quarter)
{
    bool need_reanchor = false;
    if (us_per_quarter < UPQ_MIN || us_per_quarter > UPQ_MAX) return -1;

    lock();
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) {
        const uint32_t cur_song = s_seg_song + (cur_pb_locked(now_us()) - s_seg_tick);
        if (at_song_tick < cur_song) { unlock(); return -1; }
        need_reanchor = (at_song_tick == cur_song);
    }
    {
        int i = 0;
        while (i < s_tempo_n && s_tempo[i].at_tick < at_song_tick) ++i;
        if (i < s_tempo_n && s_tempo[i].at_tick == at_song_tick) {
            s_tempo[i].upq = us_per_quarter;
        } else {
            if (s_tempo_n >= SEQCORE_TEMPO_MAX) { unlock(); return -1; }
            if (i < s_tempo_n) {
                memmove(&s_tempo[i + 1], &s_tempo[i],
                        (size_t)(s_tempo_n - i) * sizeof(TempoEntry));
            }
            s_tempo[i].at_tick = at_song_tick;
            s_tempo[i].upq = us_per_quarter;
            s_tempo_n++;
        }
    }
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) {
        if (need_reanchor) {
            const int64_t t = now_us();
            const uint32_t pb = cur_pb_locked(t);
            s_seg_song = s_seg_song + (pb - s_seg_tick);
            s_seg_tick = pb;
            s_seg_us = t;
            s_seg_upq = us_per_quarter;
        }
        /* キューの積み直しは不要(ソートキーが tick なので変換係数だけが変わる)*/
        seg_recompute_end_locked();
    }
    unlock();

    rearm();
    return 0;
}

int32_t seqcore_tempomap_set_meter(uint32_t at_song_tick, uint32_t numer, uint32_t denom)
{
    if (numer < 1 || numer > 32) return -1;
    if (denom != 1 && denom != 2 && denom != 4 && denom != 8 && denom != 16) return -1;

    lock();
    {
        int i = 0;
        while (i < s_meter_n && s_meter[i].at_tick < at_song_tick) ++i;
        if (i < s_meter_n && s_meter[i].at_tick == at_song_tick) {
            s_meter[i].numer = (uint16_t)numer;
            s_meter[i].denom = (uint16_t)denom;
        } else {
            if (s_meter_n >= SEQCORE_METER_MAX) { unlock(); return -1; }
            if (i < s_meter_n) {
                memmove(&s_meter[i + 1], &s_meter[i],
                        (size_t)(s_meter_n - i) * sizeof(MeterEntry));
            }
            s_meter[i].at_tick = at_song_tick;
            s_meter[i].numer = (uint16_t)numer;
            s_meter[i].denom = (uint16_t)denom;
            s_meter_n++;
        }
    }
    unlock();
    return 0;
}

int32_t seqcore_tempomap_set_loop(uint32_t start_song_tick, uint32_t end_song_tick)
{
    if (start_song_tick != 0 || end_song_tick != 0) {
        if (start_song_tick >= end_song_tick) return -1;
    }
    lock();
    s_loop_start = start_song_tick;
    s_loop_end = end_song_tick;
    if (s_state == HOSTAPI_TRANSPORT_PLAYING) seg_recompute_end_locked();
    unlock();
    rearm();
    return 0;
}

int32_t seqcore_seq_write(const void* buf, size_t buf_len)
{
    const hostapi_seq_event_t* src = (const hostapi_seq_event_t*)buf;
    const size_t want = buf_len / sizeof(hostapi_seq_event_t);
    int32_t n = 0;
    if (buf == NULL || want == 0) return 0;

    lock();
    while ((size_t)n < want && s_count < SEQCORE_QUEUE_DEPTH) {
        const hostapi_seq_event_t ev = src[n];
        /* 安定な挿入ソート: 同一 tick の後ろへ入れる(書き込み順を保つ)*/
        int i = s_count;
        while (i > 0 && s_queue[i - 1].tick > ev.tick) --i;
        if (i < s_count) {
            memmove(&s_queue[i + 1], &s_queue[i],
                    (size_t)(s_count - i) * sizeof(hostapi_seq_event_t));
        }
        s_queue[i] = ev;
        s_count++;
        n++;
    }
    unlock();

    if (n > 0) rearm();
    return n; /* プレフィックス受理(残りはアプリが再送する。§11-9)*/
}

int32_t seqcore_seq_flush_after(uint32_t tick)
{
    int32_t removed = 0;
    lock();
    {
        int keep = 0;
        for (int i = 0; i < s_count; ++i) {
            if (s_queue[i].tick >= tick) { removed++; continue; }
            if (keep != i) s_queue[keep] = s_queue[i];
            keep++;
        }
        s_count = keep;
    }
    unlock();

    if (removed > 0) rearm();
    return removed;
}

int32_t seqcore_seq_filled_until(void)
{
    uint32_t t;
    lock();
    if (s_count > 0) {
        t = s_queue[s_count - 1].tick;
    } else if (s_state == HOSTAPI_TRANSPORT_PLAYING) {
        t = cur_pb_locked(now_us());
    } else {
        t = s_pb_at_stop;
    }
    unlock();
    return (int32_t)t;
}

int32_t seqcore_time_us_to_tick(int64_t us)
{
    int64_t tick;
    lock();
    if (s_state != HOSTAPI_TRANSPORT_PLAYING) { unlock(); return -1; }
    tick = (int64_t)s_seg_tick + ((us - s_seg_us) * (int64_t)HOSTAPI_PPQN) / (int64_t)s_seg_upq;
    unlock();
    if (tick < 0) tick = 0;
    return (int32_t)tick;
}

/* ---- 自己検査 ---- */

int seqcore_selftest(void)
{
    static hostapi_seq_event_t bulk[SEQCORE_QUEUE_DEPTH];
    hostapi_seq_event_t evs[5];
    hostapi_seq_event_t same[4];
    int fails = 0;

#define CHECK(cond) do { if (!(cond)) fails++; } while (0)

    seqcore_reset();

    /* 1. tick 昇順に整列すること(降順で投入する)*/
    memset(evs, 0, sizeof(evs));
    for (int i = 0; i < 5; ++i) {
        evs[i].tick = (uint32_t)(4 - i) * 100u;
        evs[i].port = HOSTAPI_PORT_CLICK;
        evs[i].status = HOSTAPI_SEQ_OP_TONE;
        evs[i].data1 = (uint8_t)i;
    }
    CHECK(seqcore_seq_write(evs, sizeof(evs)) == 5);
    for (int i = 1; i < s_count; ++i) CHECK(s_queue[i - 1].tick <= s_queue[i].tick);
    CHECK(s_queue[0].tick == 0 && s_queue[4].tick == 400);

    /* 2. 同一 tick は書き込み順を保つこと */
    seqcore_reset();
    memset(same, 0, sizeof(same));
    for (int i = 0; i < 4; ++i) {
        same[i].tick = 960;
        same[i].port = HOSTAPI_PORT_CLICK;
        same[i].status = HOSTAPI_SEQ_OP_TONE;
        same[i].data1 = (uint8_t)(10 + i);
    }
    CHECK(seqcore_seq_write(same, sizeof(same)) == 4);
    for (int i = 0; i < 4; ++i) CHECK(s_queue[i].data1 == (uint8_t)(10 + i));

    /* 3. 満杯時の受理数(プレフィックス受理)*/
    seqcore_reset();
    memset(bulk, 0, sizeof(bulk));
    for (int i = 0; i < SEQCORE_QUEUE_DEPTH; ++i) bulk[i].tick = (uint32_t)i;
    CHECK(seqcore_seq_write(bulk, sizeof(bulk)) == SEQCORE_QUEUE_DEPTH);
    CHECK(seqcore_seq_write(bulk, sizeof(hostapi_seq_event_t) * 10) == 0);
    seqcore_reset();
    CHECK(seqcore_seq_write(bulk, sizeof(hostapi_seq_event_t) * (SEQCORE_QUEUE_DEPTH - 4)) ==
          SEQCORE_QUEUE_DEPTH - 4);
    CHECK(seqcore_seq_write(bulk, sizeof(hostapi_seq_event_t) * 10) == 4);

    /* 4. flush_after の件数 */
    seqcore_reset();
    CHECK(seqcore_seq_write(bulk, sizeof(hostapi_seq_event_t) * 100) == 100);
    CHECK(seqcore_seq_flush_after(60) == 40);
    CHECK(seqcore_seq_filled_until() == 59);
    CHECK(seqcore_seq_flush_after(0) == 60);

    /* 5. 端数バイトは無視される */
    seqcore_reset();
    CHECK(seqcore_seq_write(bulk, sizeof(hostapi_seq_event_t) * 2 + 7) == 2);

    seqcore_reset();
#undef CHECK
    return fails;
}
