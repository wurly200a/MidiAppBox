/*
 * MIDI OUT native 実装(Linux / ALSA シーケンサ バックエンド)。Phase 8b。
 *
 * 実機側(src/components/midi/midi.cpp)と同じ契約:
 * - hostapi_midi_send は生バイト列をそのまま送る(App drives API)。
 * - 単独の Start(0xFA)/Continue(0xFB)/Stop(0xFC) を検出したら、host 内部で
 *   24ppqn の MIDI Clock(0xF8)をタイマ駆動で生成する。テンポは新規に
 *   保持せず、既存クリックスケジューラの「直前発音時刻→次回予約時刻」の
 *   差分から都度導出する(詳細は shared/hostapi_defs.h の "midi" セクション)。
 *
 * 実機との違い: 実機はハードウェア UART。Linux ホストは実 MIDI ポートを
 * 持たないため、ALSA シーケンサ(libasound, 任意依存)で実際のポート
 * (既定では名前に "UM-ONE" を含むクライアント。MIDIBOX_MIDI_PORT で上書き
 * 可能)へ接続して送信する。ALSA が使えない/見つからない環境では、送信
 * バイト列を stderr にログ出力するだけのフォールバックで動作を継続する。
 */
#include "hostapi_midi.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#define MAX_MSG_LEN 8
#define CLOCK_PPQN 24
#define MIN_CLOCK_INTERVAL_MS 1

static bool s_ready;
static SDL_mutex* s_mutex;
static bool s_clock_running;
static uint32_t s_last_target_ms;   /* 直近に受け取った予約時刻(発火有無は問わない、0=未確定) */
static uint32_t s_next_period_ms;   /* staging: 次に使う予測テンポ(0=未確定) */
static SDL_TimerID s_clock_timer;

#ifdef HAVE_ALSA
static snd_seq_t* s_seq;
static int s_port = -1;
static snd_midi_event_t* s_codec;

/* 名前に MIDIBOX_MIDI_PORT(既定 "UM-ONE")を含む、書き込み可能なポートを
 * 探して接続する(見つからなければログを出して未接続のまま継続)。 */
static void try_connect_destination(void)
{
    const char* want = getenv("MIDIBOX_MIDI_PORT");
    if (!want || !want[0]) want = "UM-ONE";

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    const int self = snd_seq_client_id(s_seq);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(s_seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == self) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(s_seq, pinfo) >= 0) {
            const unsigned int cap = snd_seq_port_info_get_capability(pinfo);
            if (!(cap & SND_SEQ_PORT_CAP_WRITE) || !(cap & SND_SEQ_PORT_CAP_SUBS_WRITE)) {
                continue;
            }
            const char* cname = snd_seq_client_info_get_name(cinfo);
            const char* pname = snd_seq_port_info_get_name(pinfo);
            if (strstr(cname, want) || strstr(pname, want)) {
                const int dst_port = snd_seq_port_info_get_port(pinfo);
                if (snd_seq_connect_to(s_seq, s_port, client, dst_port) == 0) {
                    fprintf(stderr, "midi: connected to ALSA seq %d:%d (%s / %s)\n",
                            client, dst_port, cname, pname);
                } else {
                    fprintf(stderr, "midi: found %s but connect failed\n", cname);
                }
                return;
            }
        }
    }
    fprintf(stderr,
            "midi: no destination matching \"%s\" found (aconnect -l to check; "
            "set MIDIBOX_MIDI_PORT to override). Sending unconnected.\n", want);
}
#endif

static void midi_output_bytes(const uint8_t* bytes, size_t len)
{
#ifdef HAVE_ALSA
    if (s_seq && s_codec) {
        SDL_LockMutex(s_mutex);
        snd_midi_event_reset_encode(s_codec);
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        const long n = snd_midi_event_encode(s_codec, bytes, (long)len, &ev);
        if (n > 0 && ev.type != SND_SEQ_EVENT_NONE) {
            snd_seq_ev_set_source(&ev, s_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(s_seq, &ev);
        }
        SDL_UnlockMutex(s_mutex);
        return;
    }
#endif
    fprintf(stderr, "midi: send");
    for (size_t i = 0; i < len; i++) fprintf(stderr, " %02X", bytes[i]);
    fprintf(stderr, "\n");
}

static Uint32 clock_timer_cb(Uint32 interval, void* param)
{
    (void)param;
    const uint8_t clock_byte = 0xF8;
    midi_output_bytes(&clock_byte, 1);
    return interval; /* 同じ間隔で継続(次回のビートで再同期されるまで) */
}

bool host_midi_init(void)
{
    s_mutex = SDL_CreateMutex();
#ifdef HAVE_ALSA
    if (snd_seq_open(&s_seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "midi: snd_seq_open failed (falling back to log-only)\n");
        s_seq = NULL;
    } else {
        snd_seq_set_client_name(s_seq, "MidiAppBox");
        s_port = snd_seq_create_simple_port(s_seq, "MIDI OUT",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (s_port < 0) {
            fprintf(stderr, "midi: snd_seq_create_simple_port failed (falling back to log-only)\n");
            snd_seq_close(s_seq);
            s_seq = NULL;
        } else if (snd_midi_event_new(MAX_MSG_LEN, &s_codec) < 0) {
            fprintf(stderr, "midi: snd_midi_event_new failed (falling back to log-only)\n");
            snd_seq_close(s_seq);
            s_seq = NULL;
        } else {
            try_connect_destination();
        }
    }
#else
    fprintf(stderr, "midi: built without ALSA (log-only)\n");
#endif
    s_ready = true;
    return true;
}

void host_midi_shutdown(void)
{
    if (s_clock_timer) {
        SDL_RemoveTimer(s_clock_timer);
        s_clock_timer = 0;
    }
#ifdef HAVE_ALSA
    if (s_codec) {
        snd_midi_event_free(s_codec);
        s_codec = NULL;
    }
    if (s_seq) {
        snd_seq_close(s_seq);
        s_seq = NULL;
    }
#endif
    if (s_mutex) {
        SDL_DestroyMutex(s_mutex);
        s_mutex = NULL;
    }
    s_ready = false;
}

void host_midi_reset(void)
{
    if (!s_ready) return;
    SDL_LockMutex(s_mutex);
    s_clock_running = false;
    s_last_target_ms = 0;
    s_next_period_ms = 0;
    SDL_TimerID t = s_clock_timer;
    s_clock_timer = 0;
    SDL_UnlockMutex(s_mutex);
    if (t) SDL_RemoveTimer(t);
}

void host_midi_notify_beat_scheduled(uint32_t target_ms)
{
    if (!s_ready) return;
    SDL_LockMutex(s_mutex);
    if (s_clock_running) {
        /* 「直前に発音した時刻」ではなく「直前に受け取った予約時刻」との
         * 差分を使う。rearm()(BPM/拍子変更)は「拍0を今すぐ」を予約する
         * ため target_ms は既存の(未来の)予約より必ず小さくなり、この
         * 比較で自然に無視される。直後に続けて予約される「拍1」(新テンポ
         * での本当の次拍)は、この「拍0=今」を基準にした正しい周期になる
         * (詳細は実機側 midi.cpp の同名コメント参照)。 */
        if (s_last_target_ms != 0 && target_ms > s_last_target_ms) {
            s_next_period_ms = target_ms - s_last_target_ms;
        }
        s_last_target_ms = target_ms;
    }
    SDL_UnlockMutex(s_mutex);
}

void host_midi_notify_beat_fired(uint32_t fired_ms)
{
    (void)fired_ms; /* 位相基準は「今すぐ」で十分(通知は発音直後に呼ばれる) */
    if (!s_ready) return;

    SDL_LockMutex(s_mutex);
    const bool running = s_clock_running;
    const uint32_t period = s_next_period_ms;
    SDL_TimerID old_timer = s_clock_timer;
    s_clock_timer = 0;
    SDL_UnlockMutex(s_mutex);

    if (old_timer) SDL_RemoveTimer(old_timer);
    if (!running || period == 0) return;

    Uint32 interval_ms = period / CLOCK_PPQN;
    if (interval_ms < MIN_CLOCK_INTERVAL_MS) interval_ms = MIN_CLOCK_INTERVAL_MS;

    const SDL_TimerID nt = SDL_AddTimer(interval_ms, clock_timer_cb, NULL);
    SDL_LockMutex(s_mutex);
    s_clock_timer = nt;
    SDL_UnlockMutex(s_mutex);
}

int32_t native_hostapi_midi_send(wasm_exec_env_t exec_env, const char* bytes, uint32_t len)
{
    (void)exec_env;
    if (!s_ready || bytes == NULL || len == 0 || len > MAX_MSG_LEN) return -1;

    const uint8_t* b = (const uint8_t*)bytes;
    if (len == 1) {
        if (b[0] == 0xFA || b[0] == 0xFB) { /* Start / Continue */
            SDL_LockMutex(s_mutex);
            s_clock_running = true;
            s_last_target_ms = 0;
            s_next_period_ms = 0;
            SDL_UnlockMutex(s_mutex);
            fprintf(stderr, "midi: clock start\n");
        } else if (b[0] == 0xFC) { /* Stop */
            SDL_LockMutex(s_mutex);
            s_clock_running = false;
            s_last_target_ms = 0;
            s_next_period_ms = 0;
            SDL_TimerID t = s_clock_timer;
            s_clock_timer = 0;
            SDL_UnlockMutex(s_mutex);
            if (t) SDL_RemoveTimer(t);
            fprintf(stderr, "midi: clock stop\n");
        }
    }

    midi_output_bytes(b, len);
    return 0;
}
