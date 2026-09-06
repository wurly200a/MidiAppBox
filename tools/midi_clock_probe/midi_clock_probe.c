/*
 * midi_clock_probe — MIDI Clock 受信プローブ(Phase 13 の測定ツール)
 *
 * ALSA シーケンサから System Realtime(0xF8/0xFA/0xFB/0xFC)を受け、1 件 1 行の
 * CSV を吐くだけの受信専用ツール。集計は analyze.py が行う(生データを残して
 * おけば判定条件を変えても再測定が要らない)。
 *
 * 打刻:
 *   t_queue_us = ALSA シーケンサのカーネル側打刻(real-time キューによる
 *                タイムスタンプ)。ユーザ空間のスケジューリング遅延が乗らない。
 *                統計はこちらで採る。
 *   t_user_us  = 受信ループ内の clock_gettime(CLOCK_MONOTONIC)。
 *                両者の差を見れば「ツール自身が遅延を作っていないか」を
 *                自己検証できる(docs/workflow.md の測定手順を参照)。
 *   いずれもキュー開始時刻を 0 とする µs。
 *
 * 接続先は既定で名前に "UM-ONE" を含む読み出し可能ポート(--port で変更)。
 * ポート探索は hosts/linux/hostapi_midi.c と同じ走査。
 *
 * ビルド: cc -O2 -o midi_clock_probe midi_clock_probe.c $(pkg-config --cflags --libs alsa)
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t s_quit;

static void on_signal(int sig) { (void)sig; s_quit = 1; }

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* 名前に match を含む「読み出し可能」ポートへ接続する */
static int connect_source(snd_seq_t* seq, int my_port, const char* match,
                          char* found, size_t found_len)
{
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    const int self = snd_seq_client_id(seq);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            const unsigned int cap = snd_seq_port_info_get_capability(pinfo);
            if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) continue;
            const char* cname = snd_seq_client_info_get_name(cinfo);
            const char* pname = snd_seq_port_info_get_name(pinfo);
            if ((cname && strstr(cname, match)) || (pname && strstr(pname, match))) {
                const int src = snd_seq_port_info_get_port(pinfo);
                if (snd_seq_connect_from(seq, my_port, client, src) == 0) {
                    snprintf(found, found_len, "%d:%d %s / %s", client, src,
                             cname ? cname : "?", pname ? pname : "?");
                    return 0;
                }
            }
        }
    }
    return -1;
}

static const char* kind_of(unsigned int type)
{
    switch (type) {
    case SND_SEQ_EVENT_CLOCK:    return "clock";    /* 0xF8 */
    case SND_SEQ_EVENT_START:    return "start";    /* 0xFA */
    case SND_SEQ_EVENT_CONTINUE: return "continue"; /* 0xFB */
    case SND_SEQ_EVENT_STOP:     return "stop";     /* 0xFC */
    case SND_SEQ_EVENT_TICK:     return "tick";     /* 0xF9 */
    case SND_SEQ_EVENT_SENSING:  return "sensing";  /* 0xFE */
    default:                     return NULL;       /* ノート等は other に集約 */
    }
}

int main(int argc, char** argv)
{
    const char* out_path = NULL;
    const char* match = "UM-ONE";
    double duration_s = 0.0; /* 0 = 無制限(SIGINT で終了)*/
    double wait_s = 0.0;     /* 接続先が現れるまで待つ秒数(0 = 待たない)*/

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)           out_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)     match = argv[++i];
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc)     wait_s = atof(argv[++i]);
        else {
                fprintf(stderr,
                    "usage: %s --out <csv> [--duration <sec>] [--port <substr>] [--wait <sec>]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!out_path) {
        fprintf(stderr, "error: --out is required\n");
        return 2;
    }

    snd_seq_t* seq = NULL;
    /* DUPLEX で開く: キュー開始イベントの送出に出力側が要る
     * (INPUT だけで開くとキューが走らず、打刻が常に 0 になる)*/
    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        fprintf(stderr, "error: snd_seq_open: %s\n", snd_strerror(err));
        return 1;
    }
    snd_seq_set_client_name(seq, "midi_clock_probe");

    /* 受信キュー: カーネル側で real-time タイムスタンプを打たせる */
    const int queue = snd_seq_alloc_queue(seq);
    if (queue < 0) {
        fprintf(stderr, "error: snd_seq_alloc_queue: %s\n", snd_strerror(queue));
        return 1;
    }

    snd_seq_port_info_t* pinfo;
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_port_info_set_name(pinfo, "probe in");
    snd_seq_port_info_set_capability(pinfo, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    snd_seq_port_info_set_type(pinfo, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_port_info_set_timestamping(pinfo, 1);
    snd_seq_port_info_set_timestamp_real(pinfo, 1);
    snd_seq_port_info_set_timestamp_queue(pinfo, queue);
    err = snd_seq_create_port(seq, pinfo);
    if (err < 0) {
        fprintf(stderr, "error: snd_seq_create_port: %s\n", snd_strerror(err));
        return 1;
    }
    const int my_port = snd_seq_port_info_get_port(pinfo);

    /* 入力プールを広めに(取りこぼしをツール側で作らない)*/
    snd_seq_set_client_pool_input(seq, 2000);

    /* 接続先が現れるまで待てるようにする(測定対象を後から起動する場合)*/
    char found[256] = "";
    {
        const int64_t deadline = mono_us() + (int64_t)(wait_s * 1e6);
        for (;;) {
            if (connect_source(seq, my_port, match, found, sizeof(found)) == 0) break;
            if (mono_us() >= deadline) {
                fprintf(stderr, "error: no readable port matching \"%s\"\n", match);
                return 1;
            }
            struct timespec req = {0, 200 * 1000 * 1000};
            nanosleep(&req, NULL);
        }
    }

    FILE* fp = fopen(out_path, "w");
    if (!fp) {
        fprintf(stderr, "error: cannot open %s\n", out_path);
        return 1;
    }
    fprintf(fp, "# source=%s\n", found);
    fprintf(fp, "seq,kind,t_queue_us,t_user_us\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const int64_t t0 = mono_us();
    snd_seq_start_queue(seq, queue, NULL);
    snd_seq_drain_output(seq);

    fprintf(stderr, "probe: connected to %s\n", found);
    fprintf(stderr, "probe: writing %s (duration %.0fs, Ctrl-C to stop)\n", out_path, duration_s);

    int npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd* pfds = calloc((size_t)(npfds > 0 ? npfds : 1), sizeof(struct pollfd));
    snd_seq_poll_descriptors(seq, pfds, (unsigned)npfds, POLLIN);

    uint64_t n_rows = 0, n_clock = 0, n_start = 0, n_cont = 0, n_stop = 0, n_other = 0;
    int64_t next_report = t0 + 2000000;

    while (!s_quit) {
        if (duration_s > 0.0 && (double)(mono_us() - t0) / 1e6 >= duration_s) break;
        const int pr = poll(pfds, (unsigned)npfds, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr > 0) {
            for (;;) {
                snd_seq_event_t* ev = NULL;
                if (snd_seq_event_input(seq, &ev) < 0 || !ev) break;
                const int64_t tu = mono_us() - t0;
                const int64_t tq = (int64_t)ev->time.time.tv_sec * 1000000 +
                                   (int64_t)ev->time.time.tv_nsec / 1000;
                const char* k = kind_of(ev->type);
                if (!k) { n_other++; k = "other"; }
                else if (ev->type == SND_SEQ_EVENT_CLOCK) n_clock++;
                else if (ev->type == SND_SEQ_EVENT_START) n_start++;
                else if (ev->type == SND_SEQ_EVENT_CONTINUE) n_cont++;
                else if (ev->type == SND_SEQ_EVENT_STOP) n_stop++;
                fprintf(fp, "%llu,%s,%lld,%lld\n", (unsigned long long)n_rows, k,
                        (long long)tq, (long long)tu);
                n_rows++;
                if (snd_seq_event_input_pending(seq, 0) <= 0) break;
            }
        }
        const int64_t now = mono_us();
        if (now >= next_report) {
            next_report = now + 2000000;
            /* 計測中に外から CSV を読めるよう定期的に流す(既定の 4KB
             * バッファのままだと数十秒ぶん見えないままになる)*/
            fflush(fp);
            fprintf(stderr, "\rprobe: %6.1fs  clock=%llu start=%llu cont=%llu stop=%llu other=%llu   ",
                    (double)(now - t0) / 1e6, (unsigned long long)n_clock,
                    (unsigned long long)n_start, (unsigned long long)n_cont,
                    (unsigned long long)n_stop, (unsigned long long)n_other);
            fflush(stderr);
        }
    }

    fprintf(stderr, "\nprobe: done. rows=%llu clock=%llu start=%llu cont=%llu stop=%llu other=%llu\n",
            (unsigned long long)n_rows, (unsigned long long)n_clock,
            (unsigned long long)n_start, (unsigned long long)n_cont,
            (unsigned long long)n_stop, (unsigned long long)n_other);
    fclose(fp);
    snd_seq_stop_queue(seq, queue, NULL);
    snd_seq_drain_output(seq);
    snd_seq_close(seq);
    free(pfds);
    return 0;
}
