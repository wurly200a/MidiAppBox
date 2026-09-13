/* shared/seq_core.c の単体テスト(Phase 17)。
 *
 * 偽の時計と、送出バイトを記録するフックで seq_core をリンクし、ディスパッチを
 * 決定的に進める。実機やリアルタイムでは「境界の数 µs」を見ることになる挙動
 * (境界停止でその tick のクロックを出さない、など)を、ここで確実に検査する。
 *
 * 実行: hosts/linux で `cmake --build build && ctest --test-dir build --output-on-failure`
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "seq_core.h"

/* ---- 偽のフック ---- */

static int64_t g_now = 1000000;
static bool g_armed;
static int64_t g_arm_at;

typedef struct {
    int64_t us;
    uint8_t b;
} OutByte;
#define OUT_MAX 16384
static OutByte g_out[OUT_MAX];
static int g_out_n;

static int64_t fake_now(void) { return g_now; }
static void fake_lock(void) {}
static void fake_unlock(void) {}
static void fake_arm(int64_t delay_us)
{
    g_armed = true;
    g_arm_at = g_now + (delay_us < 0 ? 0 : delay_us);
}
static void fake_disarm(void) { g_armed = false; }
static void fake_send(const uint8_t* bytes, size_t len)
{
    for (size_t i = 0; i < len && g_out_n < OUT_MAX; ++i) {
        g_out[g_out_n].us = g_now;
        g_out[g_out_n].b = bytes[i];
        g_out_n++;
    }
}
static void fake_click(uint32_t slot) { (void)slot; }

static const seqcore_hooks_t k_hooks = {
    fake_now, fake_lock, fake_unlock, fake_arm, fake_disarm, fake_send, fake_click,
};

/* ---- 補助 ---- */

static int g_fails;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            g_fails++;                                                            \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __func__, __LINE__, #c);        \
        }                                                                         \
    } while (0)

#define PPQN 960u
#define BAR44 (PPQN * 4u)

/* 時計を t まで進め、その間に来るディスパッチをすべて実行する */
static void run_until(int64_t t)
{
    while (g_armed && g_arm_at <= t) {
        if (g_arm_at > g_now) g_now = g_arm_at;
        g_armed = false;
        seqcore_dispatch();
    }
    if (t > g_now) g_now = t;
}

static hostapi_position_t pos(void)
{
    hostapi_position_t p;
    seqcore_transport_get_position(&p, sizeof(p));
    return p;
}

/* 1ms 刻みで、playback tick / song tick が target に達するまで進める */
static bool run_to_pb(uint32_t target)
{
    for (int i = 0; i < 4000000; ++i) {
        if (pos().tick >= target) return true;
        run_until(g_now + 1000);
    }
    return false;
}

static bool run_to_song(uint32_t target)
{
    for (int i = 0; i < 4000000; ++i) {
        if (pos().song_tick >= target) return true;
        run_until(g_now + 1000);
    }
    return false;
}

static int index_of(uint8_t b, int from)
{
    for (int i = from; i < g_out_n; ++i) {
        if (g_out[i].b == b) return i;
    }
    return -1;
}

static int count_byte(uint8_t b, int from, int to)
{
    int n = 0;
    for (int i = from; i < to && i < g_out_n; ++i) {
        if (g_out[i].b == b) n++;
    }
    return n;
}

static void reset_all(void)
{
    seqcore_reset();
    g_out_n = 0;
    g_armed = false;
    g_now += 1000000;
}

static hostapi_seq_event_t ev(uint32_t tick, uint8_t port, uint8_t status, uint8_t d1, uint8_t d2)
{
    hostapi_seq_event_t e;
    memset(&e, 0, sizeof(e));
    e.tick = tick;
    e.port = port;
    e.status = status;
    e.data1 = d1;
    e.data2 = d2;
    return e;
}

/* ---- テスト ---- */

static void test_existing_selftest(void)
{
    CHECK(seqcore_selftest() == 0);
}

static void test_clear(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 250000) == 0);
    CHECK(seqcore_tempomap_set_meter(0, 3, 4) == 0);
    CHECK(seqcore_tempomap_set_meter(2880, 5, 8) == 0);
    CHECK(seqcore_tempomap_set_loop(0, 2880) == 0);
    CHECK(seqcore_tempomap_clear() == 0);
    CHECK(pos().tempo_upq == 500000); /* 既定値に戻る */

    CHECK(seqcore_transport_start() == 0);
    CHECK(seqcore_tempomap_clear() == -1); /* PLAYING 中は不可 */
    CHECK(run_to_pb(BAR44 * 2 + 10));
    hostapi_position_t p = pos();
    CHECK(p.bar == 2 && p.beat == 0);    /* 4/4 に戻っている */
    CHECK(p.song_tick == p.tick);        /* ループも解除されている */
    CHECK(p.tempo_upq == 500000);
    seqcore_transport_stop();
}

/* V2: 再生を始め直しても前回の予約が混ざらない */
static void test_restart_after_clear(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_transport_start() == 0);
    CHECK(seqcore_tempomap_set_tempo(BAR44, 250000) == 0);
    CHECK(seqcore_tempomap_set_meter(BAR44, 3, 4) == 0);
    CHECK(run_to_pb(1000));
    CHECK(seqcore_transport_stop() == 0);

    /* clear しないと、前回の未来の予約が残っている(Phase 16 H8 の再現)*/
    CHECK(seqcore_transport_start() == 0);
    CHECK(run_to_song(BAR44 + 100));
    CHECK(pos().tempo_upq == 250000);
    CHECK(seqcore_transport_stop() == 0);

    /* clear してから初期値を設定して始めれば混ざらない(P6 の手順のまま)*/
    CHECK(seqcore_tempomap_clear() == 0);
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == 0);
    CHECK(seqcore_transport_start() == 0);
    CHECK(run_to_song(BAR44 + 100));
    hostapi_position_t p = pos();
    CHECK(p.tempo_upq == 500000);
    CHECK(p.bar == 1 && p.beat == 0);
    seqcore_transport_stop();
}

/* V1(テンポ): 上限 32 件の 3 倍以上の変化を通過させても -1 にならない */
static void test_tempo_compaction(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_transport_start() == 0);
    for (uint32_t n = 1; n <= 100; ++n) {
        const uint32_t at = n * BAR44;
        const uint32_t upq = (n % 2) ? 250000u : 400000u;
        const int32_t r = seqcore_tempomap_set_tempo(at, upq);
        CHECK(r == 0);
        if (r != 0) break;
        CHECK(run_to_song(at + 10));
        hostapi_position_t p = pos();
        CHECK(p.tempo_upq == upq);
        CHECK(p.bar == n);
    }
    seqcore_transport_stop();
}

/* V1(拍子): 畳み込み後も小節番号がずれない。畳んだ区間より前へは書けない */
static void test_meter_compaction_keeps_bar_numbers(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 100000) == 0);
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == 0);
    CHECK(seqcore_transport_start() == 0);
    uint32_t at = BAR44;
    for (uint32_t n = 1; n <= 100; ++n) {
        const uint32_t num = (n % 2) ? 3u : 5u;
        const uint32_t den = (n % 2) ? 4u : 8u;
        const int32_t r = seqcore_tempomap_set_meter(at, num, den);
        CHECK(r == 0);
        if (r != 0) break;
        CHECK(run_to_song(at + 5));
        hostapi_position_t p = pos();
        CHECK(p.bar == n);
        CHECK(p.beat == 0);
        at += PPQN * 4u / den * num;
    }
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == -1); /* 起点より前 */
    seqcore_transport_stop();
    /* clear で起点も戻る */
    CHECK(seqcore_tempomap_clear() == 0);
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == 0);
}

/* ループで戻る区間のエントリは畳まない(畳めなければ従来どおり -1)*/
static void test_loop_protects_entries(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_tempomap_set_tempo(BAR44, 250000) == 0);
    CHECK(seqcore_tempomap_set_loop(0, BAR44 * 2) == 0);
    for (uint32_t i = 0; i < 30; ++i) {
        CHECK(seqcore_tempomap_set_tempo(BAR44 * (3 + i), 300000) == 0); /* 満杯にする */
    }
    CHECK(seqcore_transport_start() == 0);
    CHECK(run_to_pb(BAR44 * 5)); /* 2 周以上 */
    CHECK(seqcore_tempomap_set_tempo(BAR44 * 40, 300000) == -1);

    /* 2 小節目(ループ内)のテンポが残っている */
    bool seen_second = false;
    for (int i = 0; i < 10000 && !seen_second; ++i) {
        hostapi_position_t p = pos();
        if (p.song_tick >= BAR44 + 100 && p.song_tick < BAR44 * 2 - 100) {
            CHECK(p.tempo_upq == 250000);
            seen_second = true;
        }
        run_until(g_now + 1000);
    }
    CHECK(seen_second);
    seqcore_transport_stop();
}

/* V3: 境界停止。その tick のクロックは出さず、同 tick の先行イベントは出す */
static void test_stop_on_boundary(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == 0);
    CHECK(seqcore_transport_start() == 0);

    const uint32_t T = BAR44 * 3; /* 11520 */
    hostapi_seq_event_t evs[4] = {
        ev(T, HOSTAPI_PORT_DIN_OUT, 0x90, 60, 100),       /* 先に書いた: 送出する */
        ev(T, HOSTAPI_PORT_DIN_OUT, HOSTAPI_SEQ_OP_STOP, 0, 0),
        ev(T, HOSTAPI_PORT_DIN_OUT, 0x80, 60, 0),         /* 後に書いた: 破棄 */
        ev(T + 40, HOSTAPI_PORT_DIN_OUT, 0x90, 62, 100),  /* 後の tick: 破棄 */
    };
    CHECK(seqcore_seq_write(evs, sizeof(evs)) == 4);
    run_until(g_now + 10 * 1000000);

    hostapi_position_t p = pos();
    CHECK(p.state == HOSTAPI_TRANSPORT_STOPPED);
    CHECK(p.tick == T);
    CHECK(p.song_tick == T);

    CHECK(g_out_n > 0 && g_out[0].b == 0xFA);
    const int fc = index_of(0xFC, 0);
    CHECK(fc > 0);
    CHECK(count_byte(0xF8, 0, fc) == (int)(T / 40)); /* 288 発ちょうど */
    CHECK(count_byte(0xF8, fc, g_out_n) == 0);        /* Stop の後は 0 発 */
    const int on = index_of(0x90, 0);
    CHECK(on > 0 && on < fc && g_out[on + 1].b == 60);
    CHECK(index_of(0x80, 0) < 0);
    CHECK(count_byte(0x90, 0, g_out_n) == 1);
    CHECK(fc == g_out_n - 1); /* 0xFC が最後 */

    /* continue は停止点から。停止 tick のクロックがここで初めて出る */
    g_out_n = 0;
    CHECK(seqcore_transport_continue() == 0);
    run_until(g_now + 100000);
    CHECK(g_out_n > 0 && g_out[0].b == 0xFB);
    CHECK(count_byte(0xF8, 0, g_out_n) >= 4);
    CHECK(pos().song_tick >= T);
    seqcore_transport_stop();
}

/* 予約した停止は普通のイベントなので flush で取り消せる */
static void test_stop_is_cancelled_by_flush(void)
{
    reset_all();
    CHECK(seqcore_transport_start() == 0);
    hostapi_seq_event_t e = ev(BAR44, 0, HOSTAPI_SEQ_OP_STOP, 0, 0);
    CHECK(seqcore_seq_write(&e, sizeof(e)) == 1);
    CHECK(seqcore_seq_flush_after(BAR44) == 1);
    run_until(g_now + 3 * 1000000);
    CHECK(pos().state == HOSTAPI_TRANSPORT_PLAYING);
    CHECK(index_of(0xFC, 0) < 0);
    seqcore_transport_stop();
}

/* 過去の tick に書いた停止: 直ちに止まり、playback tick は戻らない */
static void test_late_stop_keeps_playback_tick_monotonic(void)
{
    reset_all();
    CHECK(seqcore_transport_start() == 0);
    CHECK(run_to_pb(5000));
    hostapi_seq_event_t e = ev(1000, 0, HOSTAPI_SEQ_OP_STOP, 0, 0);
    CHECK(seqcore_seq_write(&e, sizeof(e)) == 1);
    run_until(g_now + 10000);

    hostapi_position_t p = pos();
    CHECK(p.state == HOSTAPI_TRANSPORT_STOPPED);
    CHECK(p.song_tick == 1000);
    const int fc = index_of(0xFC, 0);
    const int clocks = count_byte(0xF8, 0, fc);
    CHECK(clocks > 0);
    CHECK(p.tick > (uint32_t)(clocks - 1) * 40u); /* 最後に出したクロックより後 */

    /* continue してもクロックの tick が重複しない(次のグリッドから)*/
    g_out_n = 0;
    CHECK(seqcore_transport_continue() == 0);
    CHECK(pos().tick >= p.tick);
    seqcore_transport_stop();
}

/* 既存の使い方(metronome: PLAYING 中に at_tick=0 を上書き → locate(0))は変わらない */
static void test_metronome_style_usage_unchanged(void)
{
    reset_all();
    CHECK(seqcore_tempomap_set_tempo(0, 500000) == 0);
    CHECK(seqcore_tempomap_set_meter(0, 4, 4) == 0);
    CHECK(seqcore_transport_start() == 0);
    CHECK(run_to_pb(5000));
    CHECK(seqcore_tempomap_set_meter(0, 3, 4) == 0); /* 過去 tick への上書き */
    CHECK(seqcore_transport_locate(0) == 0);
    CHECK(seqcore_tempomap_set_tempo(0, 400000) == 0);
    CHECK(run_to_song(PPQN * 3 + 10));
    hostapi_position_t p = pos();
    CHECK(p.tempo_upq == 400000);
    CHECK(p.bar == 1 && p.beat == 0); /* 3/4 */
    seqcore_transport_stop();
}

int main(void)
{
    static const struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"existing_selftest", test_existing_selftest},
        {"clear", test_clear},
        {"restart_after_clear", test_restart_after_clear},
        {"tempo_compaction", test_tempo_compaction},
        {"meter_compaction_keeps_bar_numbers", test_meter_compaction_keeps_bar_numbers},
        {"loop_protects_entries", test_loop_protects_entries},
        {"stop_on_boundary", test_stop_on_boundary},
        {"stop_is_cancelled_by_flush", test_stop_is_cancelled_by_flush},
        {"late_stop_keeps_playback_tick_monotonic", test_late_stop_keeps_playback_tick_monotonic},
        {"metronome_style_usage_unchanged", test_metronome_style_usage_unchanged},
    };
    seqcore_init(&k_hooks);
    int failed_tests = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        const int before = g_fails;
        tests[i].fn();
        const bool ok = (g_fails == before);
        if (!ok) failed_tests++;
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
    }
    printf("seq_core_test: %d/%zu passed\n",
           (int)(sizeof(tests) / sizeof(tests[0])) - failed_tests,
           sizeof(tests) / sizeof(tests[0]));
    return failed_tests == 0 ? 0 : 1;
}
