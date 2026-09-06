/*
 * MIDI OUT / MIDI IN native 実装(Linux / ALSA シーケンサ バックエンド)。
 * Phase 8b(OUT)/ Phase 9a(IN)。
 *
 * 実機側(src/components/midi/midi.cpp)と同じ契約:
 * - hostapi_midi_send は生バイト列をそのまま送る(App drives API)。副作用は
 *   ない(MIDI Clock 生成は host の音楽時間軸 API が担う。Phase 14)。
 * - hostapi_midi_recv は受信した生バイトをタイムスタンプ付きでそのまま
 *   返す(パースはしない)。ホスト内部のリングバッファは 256 件、溢れたら
 *   最古を捨てる(詳細は shared/hostapi_defs.h の "midi" セクション)。
 *
 * 実機との違い: 実機はハードウェア UART。Linux ホストは実 MIDI ポートを
 * 持たないため、ALSA シーケンサ(libasound, 任意依存)で実際のポート
 * (既定では名前に "UM-ONE" を含むクライアント。MIDIBOX_MIDI_PORT で上書き
 * 可能)へ接続して送受信する。ALSA が使えない/見つからない環境では、送信は
 * バイト列を stderr にログ出力するだけ、受信は常に 0 件を返すフォールバック
 * で動作を継続する。
 */
#include "hostapi_midi.h"
#include "hostapi_defs.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#include <poll.h>
#endif

#define MAX_MSG_LEN 8
#define RX_QUEUE_DEPTH 256 /* 実機側(midi.cpp)と同じ深さ */

static bool s_ready;
static SDL_mutex* s_mutex;

/* ---- MIDI IN 受信リングバッファ(Phase 9a) ----
 * 生産者は rx_thread(ALSA seq input を待つ専用スレッド)、消費者は
 * native_hostapi_midi_recv(wasm アプリスレッド)。s_mutex を共用する
 * (送信側とは別の状態なので競合は起きないが、実装を単純に保つため
 * 単一ミューテックスに統一)。 */
static hostapi_midi_recv_t s_rxq[RX_QUEUE_DEPTH];
static int s_rxq_head;
static int s_rxq_count;
static Uint64 s_perf_freq;   /* SDL_GetPerformanceFrequency() */
static Uint64 s_perf_epoch;  /* host_midi_init 時点の SDL_GetPerformanceCounter() */

/* 起動基準の単調増加 µs クロック(実機の esp_timer_get_time() 相当。
 * epoch は実機とは異なるが、レコード間の差分計算にのみ使う前提)。 */
static uint64_t now_us(void)
{
    const Uint64 c = SDL_GetPerformanceCounter() - s_perf_epoch;
    return (uint64_t)((c * (Uint64)1000000) / s_perf_freq);
}

static void rxq_reset_locked(void)
{
    s_rxq_head = 0;
    s_rxq_count = 0;
}

static void rxq_push(uint8_t byte, uint64_t timestamp_us)
{
    bool dropped = false;
    SDL_LockMutex(s_mutex);
    if (s_rxq_count == RX_QUEUE_DEPTH) { /* 満杯: 最古を捨てる */
        s_rxq_head = (s_rxq_head + 1) % RX_QUEUE_DEPTH;
        s_rxq_count--;
        dropped = true;
    }
    hostapi_midi_recv_t* rec = &s_rxq[(s_rxq_head + s_rxq_count) % RX_QUEUE_DEPTH];
    rec->timestamp_us = timestamp_us;
    rec->byte = byte;
    memset(rec->_reserved, 0, sizeof(rec->_reserved));
    s_rxq_count++;
    SDL_UnlockMutex(s_mutex);

    if (dropped) fprintf(stderr, "midi: RX ring buffer full, dropped oldest record\n");
}

#ifdef HAVE_ALSA
static snd_seq_t* s_seq;
static int s_port = -1;
static int s_in_port = -1;
static snd_midi_event_t* s_codec;
static snd_midi_event_t* s_decode_codec;
static SDL_Thread* s_rx_thread;
static volatile bool s_rx_thread_stop;

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

/* 名前に MIDIBOX_MIDI_PORT(既定 "UM-ONE")を含む、読み出し可能なポートを
 * 探して自ポート(MIDI IN)へ接続する(見つからなければログを出して未接続
 * のまま継続。受信は常に 0 件になる)。 */
static void try_connect_source(void)
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
            if (!(cap & SND_SEQ_PORT_CAP_READ) || !(cap & SND_SEQ_PORT_CAP_SUBS_READ)) {
                continue;
            }
            const char* cname = snd_seq_client_info_get_name(cinfo);
            const char* pname = snd_seq_port_info_get_name(pinfo);
            if (strstr(cname, want) || strstr(pname, want)) {
                const int src_port = snd_seq_port_info_get_port(pinfo);
                if (snd_seq_connect_from(s_seq, s_in_port, client, src_port) == 0) {
                    fprintf(stderr, "midi: MIDI IN connected to ALSA seq %d:%d (%s / %s)\n",
                            client, src_port, cname, pname);
                } else {
                    fprintf(stderr, "midi: found %s but MIDI IN connect failed\n", cname);
                }
                return;
            }
        }
    }
    fprintf(stderr,
            "midi: no MIDI IN source matching \"%s\" found (aconnect -l to check; "
            "set MIDIBOX_MIDI_PORT to override). hostapi_midi_recv will return 0 records.\n",
            want);
}

/* ALSA seq からの受信を待つ専用スレッド(Phase 9a)。poll() で readable を
 * 待ってから snd_seq_event_input で吸い出す(snd_seq_nonblock を全体には
 * 適用せず、このスレッドだけが read 側を扱う)。パースはせず、デコードした
 * 生バイトをタイムスタンプ付きでリングバッファへ積むだけ。 */
static int rx_thread_fn(void* arg)
{
    (void)arg;
    struct pollfd pfds[8];
    int npfds = snd_seq_poll_descriptors_count(s_seq, POLLIN);
    if (npfds <= 0 || npfds > (int)(sizeof(pfds) / sizeof(pfds[0]))) npfds = 0;
    if (npfds > 0) snd_seq_poll_descriptors(s_seq, pfds, npfds, POLLIN);

    uint8_t decode_buf[512];
    while (!s_rx_thread_stop) {
        if (npfds <= 0) {
            SDL_Delay(200);
            continue;
        }
        const int pret = poll(pfds, npfds, 200);
        if (pret <= 0) continue;

        for (;;) {
            snd_seq_event_t* ev = NULL;
            if (snd_seq_event_input(s_seq, &ev) < 0 || !ev) break;
            const uint64_t ts = now_us();
            snd_midi_event_reset_decode(s_decode_codec);
            const long n = snd_midi_event_decode(s_decode_codec, decode_buf, sizeof(decode_buf), ev);
            for (long i = 0; i < n; i++) rxq_push(decode_buf[i], ts);
            if (snd_seq_event_input_pending(s_seq, 0) <= 0) break;
        }
    }
    return 0;
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

uint64_t host_midi_now_us(void)
{
    return now_us();
}

void host_midi_tx_bytes(const uint8_t* bytes, size_t len)
{
    if (bytes == NULL || len == 0 || len > 8) return;
    midi_output_bytes(bytes, len);
}

bool host_midi_init(void)
{
    s_mutex = SDL_CreateMutex();
    s_perf_freq = SDL_GetPerformanceFrequency();
    s_perf_epoch = SDL_GetPerformanceCounter();
#ifdef HAVE_ALSA
    if (snd_seq_open(&s_seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "midi: snd_seq_open failed (falling back to log-only)\n");
        s_seq = NULL;
    } else {
        snd_seq_set_client_name(s_seq, "MidiAppBox");
        s_port = snd_seq_create_simple_port(s_seq, "MIDI OUT",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        s_in_port = snd_seq_create_simple_port(s_seq, "MIDI IN",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (s_port < 0 || s_in_port < 0) {
            fprintf(stderr, "midi: snd_seq_create_simple_port failed (falling back to log-only)\n");
            snd_seq_close(s_seq);
            s_seq = NULL;
        } else {
            if (snd_midi_event_new(MAX_MSG_LEN, &s_codec) < 0) {
                fprintf(stderr, "midi: snd_midi_event_new failed (send falls back to log-only)\n");
            } else {
                try_connect_destination();
            }
            if (snd_midi_event_new(512, &s_decode_codec) < 0) {
                fprintf(stderr, "midi: snd_midi_event_new (decode) failed (MIDI IN disabled)\n");
            } else {
                try_connect_source();
                s_rx_thread_stop = false;
                s_rx_thread = SDL_CreateThread(rx_thread_fn, "midi_rx", NULL);
            }
        }
    }
#else
    fprintf(stderr, "midi: built without ALSA (send: log-only, recv: always 0 records)\n");
#endif
    s_ready = true;
    return true;
}

void host_midi_shutdown(void)
{
#ifdef HAVE_ALSA
    if (s_rx_thread) {
        s_rx_thread_stop = true;
        SDL_WaitThread(s_rx_thread, NULL);
        s_rx_thread = NULL;
    }
    if (s_decode_codec) {
        snd_midi_event_free(s_decode_codec);
        s_decode_codec = NULL;
    }
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
    rxq_reset_locked();
    SDL_UnlockMutex(s_mutex);
}

int32_t native_hostapi_midi_send(wasm_exec_env_t exec_env, const char* bytes, uint32_t len)
{
    (void)exec_env;
    if (!s_ready || bytes == NULL || len == 0 || len > MAX_MSG_LEN) return -1;
    midi_output_bytes((const uint8_t*)bytes, len);
    return 0;
}

int32_t native_hostapi_midi_recv(wasm_exec_env_t exec_env, char* buf, uint32_t len)
{
    (void)exec_env;
    if (!s_ready) return 0;

    const uint32_t max_records = len / (uint32_t)sizeof(hostapi_midi_recv_t);
    hostapi_midi_recv_t* out = (hostapi_midi_recv_t*)buf;
    int32_t n = 0;

    SDL_LockMutex(s_mutex);
    while ((uint32_t)n < max_records && s_rxq_count > 0) {
        out[n] = s_rxq[s_rxq_head];
        s_rxq_head = (s_rxq_head + 1) % RX_QUEUE_DEPTH;
        s_rxq_count--;
        n++;
    }
    SDL_UnlockMutex(s_mutex);
    return n;
}
