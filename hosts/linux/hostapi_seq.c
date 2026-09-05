/* 音楽時間軸 API(Phase 11)の Linux ホスト実装。詳細は hostapi_seq.h。 */
#include "hostapi_seq.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hostapi_midi.h"
#include "hostapi_sdl.h"
#include "seq_core.h"

/* ---- Clock Authority(Linux 実装)----
 * 時刻源は hostapi_midi.c の単調増加 µs(hostapi_midi_recv のタイムスタンプと
 * 同一時基)。これにより time_us_to_tick が受信打刻をそのまま変換できる。
 * レートマスター(SDL の累計フレーム数)はアンカーと ppm 監視に使う。 */
#define CLOCK_RATE_HZ 44100

static atomic_ullong s_audio_frames;
static _Atomic int64_t s_anchor_us;
static atomic_ullong s_anchor_frames;
static atomic_bool s_anchored;
/* host_seq_init 前に SDL のオーディオコールバックが走る(host_sdl_init が
 * host_midi_init/host_seq_init より先に音声デバイスを開くため)。その時点では
 * 時刻源のエポックも seqcore も未初期化なので、準備できるまで無視する。 */
static atomic_bool s_ready;

static int64_t clock_now_us(void)
{
    return (int64_t)host_midi_now_us();
}

void host_seq_on_audio(uint32_t frames)
{
    if (!atomic_load(&s_ready)) return;
    const unsigned long long total =
        atomic_fetch_add(&s_audio_frames, (unsigned long long)frames) + frames;
    if (!atomic_load(&s_anchored)) {
        atomic_store(&s_anchor_frames, total);
        atomic_store(&s_anchor_us, clock_now_us());
        atomic_store(&s_anchored, true);
    }
}

/* 診断: アンカー以降の実効レート偏差(ppm)。ブラウザホストではこの値で
 * 比を補正することになる(v1 の実機/Linux は固定比で足りる)。 */
static int32_t clock_estimated_ppm(void)
{
    if (!atomic_load(&s_anchored)) return 0;
    const long long df =
        (long long)atomic_load(&s_audio_frames) - (long long)atomic_load(&s_anchor_frames);
    const int64_t elapsed = clock_now_us() - atomic_load(&s_anchor_us);
    if (elapsed < 1000000) return 0;
    const long long eff_milli = df * 1000000LL * 1000LL / elapsed;
    const long long nom_milli = (long long)CLOCK_RATE_HZ * 1000LL;
    return (int32_t)((eff_milli - nom_milli) * 1000000LL / nom_milli);
}

/* ---- 排他(L0/L1 専用。SDL のオーディオロックや描画とは共有しない。§6)---- */
static pthread_mutex_t s_core_mux = PTHREAD_MUTEX_INITIALIZER;
static void hook_lock(void) { pthread_mutex_lock(&s_core_mux); }
static void hook_unlock(void) { pthread_mutex_unlock(&s_core_mux); }

/* ---- ディスパッチスレッド ----
 * 実機は esp_timer のワンショット 1 本。Linux は専用スレッド 1 本を
 * pthread_cond_timedwait(CLOCK_MONOTONIC、µs 分解能)で絶対時刻まで眠らせる。
 * SDL_AddTimer は ms 分解能しかなく、20833µs のクロックグリッドを表現できない。 */
static pthread_mutex_t s_arm_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_arm_cv;
static pthread_t s_thread;
static bool s_thread_started;
static bool s_armed;
static bool s_quit;
static int64_t s_deadline_us;
/* clock_now_us() の epoch と CLOCK_MONOTONIC の差(絶対時刻での待機に使う) */
static int64_t s_mono_offset_us;

static int64_t mono_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void* dispatch_thread(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&s_arm_mux);
    while (!s_quit) {
        if (!s_armed) {
            pthread_cond_wait(&s_arm_cv, &s_arm_mux);
            continue;
        }
        const int64_t remain = s_deadline_us - clock_now_us();
        if (remain > 0) {
            const int64_t abs_us = mono_now_us() + remain;
            struct timespec ts;
            ts.tv_sec = (time_t)(abs_us / 1000000);
            ts.tv_nsec = (long)((abs_us % 1000000) * 1000);
            pthread_cond_timedwait(&s_arm_cv, &s_arm_mux, &ts);
            continue; /* 起こされた理由によらず条件を評価し直す */
        }
        s_armed = false;
        pthread_mutex_unlock(&s_arm_mux);
        seqcore_dispatch(); /* ロックの外(seqcore が hook_arm を呼ぶ) */
        pthread_mutex_lock(&s_arm_mux);
    }
    pthread_mutex_unlock(&s_arm_mux);
    return NULL;
}

static void hook_arm(int64_t delay_us)
{
    if (delay_us < 0) delay_us = 0;
    pthread_mutex_lock(&s_arm_mux);
    s_deadline_us = clock_now_us() + delay_us;
    s_armed = true;
    pthread_cond_signal(&s_arm_cv);
    pthread_mutex_unlock(&s_arm_mux);
}

static void hook_disarm(void)
{
    pthread_mutex_lock(&s_arm_mux);
    s_armed = false;
    pthread_cond_signal(&s_arm_cv);
    pthread_mutex_unlock(&s_arm_mux);
}

/* ---- ポート ---- */
static void hook_send_midi(const uint8_t* bytes, size_t len)
{
    host_midi_tx_bytes(bytes, len);
}

static void hook_click(uint32_t slot)
{
    host_click_play_slot(slot);
}

static const seqcore_hooks_t k_hooks = {
    clock_now_us, hook_lock, hook_unlock, hook_arm,
    hook_disarm, hook_send_midi, hook_click,
};

void host_seq_init(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&s_arm_cv, &attr);
    pthread_condattr_destroy(&attr);
    s_mono_offset_us = mono_now_us() - clock_now_us();

    seqcore_init(&k_hooks);
    atomic_store(&s_ready, true);

    s_quit = false;
    if (pthread_create(&s_thread, NULL, dispatch_thread, NULL) == 0) {
        s_thread_started = true;
    } else {
        fprintf(stderr, "seq: dispatch thread create failed\n");
    }
}

void host_seq_shutdown(void)
{
    if (!s_thread_started) return;
    pthread_mutex_lock(&s_arm_mux);
    s_quit = true;
    pthread_cond_signal(&s_arm_cv);
    pthread_mutex_unlock(&s_arm_mux);
    pthread_join(s_thread, NULL);
    s_thread_started = false;
}

void host_seq_reset(void)
{
    seqcore_reset();
    (void)clock_estimated_ppm; /* 診断用。v1 の固定比では使わない */
}

/* ---- natives(WAMR 境界の薄いラッパ)---- */

int32_t native_hostapi_transport_start(wasm_exec_env_t e)
{ (void)e; return seqcore_transport_start(); }

int32_t native_hostapi_transport_stop(wasm_exec_env_t e)
{ (void)e; return seqcore_transport_stop(); }

int32_t native_hostapi_transport_continue(wasm_exec_env_t e)
{ (void)e; return seqcore_transport_continue(); }

int32_t native_hostapi_transport_locate(wasm_exec_env_t e, int32_t song_tick)
{ (void)e; return seqcore_transport_locate((uint32_t)song_tick); }

int32_t native_hostapi_transport_get_position(wasm_exec_env_t e, char* buf, uint32_t len)
{ (void)e; return seqcore_transport_get_position(buf, len); }

int32_t native_hostapi_tempomap_set_tempo(wasm_exec_env_t e, int32_t at_tick, int32_t upq)
{ (void)e; return seqcore_tempomap_set_tempo((uint32_t)at_tick, (uint32_t)upq); }

int32_t native_hostapi_tempomap_set_meter(wasm_exec_env_t e, int32_t at_tick,
                                          int32_t numer, int32_t denom)
{
    (void)e;
    if (numer < 0 || denom < 0) return -1;
    return seqcore_tempomap_set_meter((uint32_t)at_tick, (uint32_t)numer, (uint32_t)denom);
}

int32_t native_hostapi_tempomap_set_loop(wasm_exec_env_t e, int32_t start, int32_t end)
{ (void)e; return seqcore_tempomap_set_loop((uint32_t)start, (uint32_t)end); }

int32_t native_hostapi_seq_write(wasm_exec_env_t e, const char* buf, uint32_t len)
{ (void)e; return seqcore_seq_write(buf, len); }

int32_t native_hostapi_seq_flush_after(wasm_exec_env_t e, int32_t tick)
{ (void)e; return seqcore_seq_flush_after((uint32_t)tick); }

int32_t native_hostapi_seq_filled_until(wasm_exec_env_t e)
{ (void)e; return seqcore_seq_filled_until(); }

int32_t native_hostapi_time_us_to_tick(wasm_exec_env_t e, int64_t us)
{ (void)e; return seqcore_time_us_to_tick(us); }
