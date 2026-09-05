#pragma once
/*
 * L0 / L1 コア(移植可能な C 実装)。Phase 11。
 * 設計は docs/architecture.md §4〜§7 / §9、仕様は docs/hostapi-next.md。
 *
 * 実機ホストと Linux ホストが**同一のコード**を使う。プラットフォーム依存
 * (時刻源・排他・タイマ・ポート出力)はすべて seqcore_hooks_t に外出しして
 * あり、コア自身は OS API を一切呼ばない。これは「同一の .wasm が実機と
 * Linux で同じ挙動になる」ことを構造的に保証するためであり、同時に
 * Clock Authority の抽象が実機都合に引きずられていないことの検証でもある
 * (ブラウザホスト = Phase A への移植点)。
 *
 * 責務の境界:
 *   L1 = テンポ/拍子マップ、transport 状態機械、playback tick ↔ song tick の
 *        写像(ループは剰余)、MIDI クロックの 40 tick グリッド生成。
 *   L0 = tick 昇順のイベントキュー、ワンショット 1 本のディスパッチ、
 *        ポート抽象。音楽的意味の解釈はしない。
 *
 * 時間の扱い(重要):
 *   再アームは常に**絶対時刻グリッド基準**(予定 tick → host µs)である。
 *   発火時刻からの相対加算は行わない(累積ドリフト防止。P10-3 で実証)。
 *
 * ロック規律(§6、必須):
 *   hooks->lock/unlock は L0/L1 専用の短いロックであること。LVGL /
 *   ファイルシステム / オーディオ書き込みのロックと共有してはならない。
 *   コアは臨界区間の中でポート出力・タイマ操作・ログを一切行わない。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hostapi_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI Clock のグリッド(960 / 24) */
#define SEQCORE_CLOCK_GRID_TICKS (HOSTAPI_PPQN / 24)

/* キュー深さ = 4KB / 16B(根拠は architecture.md §9)*/
#define SEQCORE_QUEUE_DEPTH 256
#define SEQCORE_TEMPO_MAX 32
#define SEQCORE_METER_MAX 32

typedef struct {
    /* 音楽時間軸の現在時刻(µs、単調増加)。Clock Authority が供給する */
    int64_t (*now_us)(void);
    /* L0/L1 専用の短い排他。ネストしない */
    void (*lock)(void);
    void (*unlock)(void);
    /* ワンショットの再アーム(delay_us >= 0)。ロックの外から呼ばれる。
     * 実装は「既存のアームを取り消して delay_us 後に seqcore_dispatch() を
     * 呼ぶ」こと。ホストのタイマ/スレッドに委ねる */
    void (*arm)(int64_t delay_us);
    /* アームの取り消し(transport 停止時)。ロックの外から呼ばれる */
    void (*disarm)(void);
    /* DIN_OUT ポートへの送出。len は 1..3(§7 の送出規律により小さく保つ) */
    void (*send_midi)(const uint8_t* bytes, size_t len);
    /* CLICK ポートの発音(トーンパレットのスロット) */
    void (*click)(uint32_t slot);
} seqcore_hooks_t;

/* 起動時に 1 回。hooks は静的寿命であること */
void seqcore_init(const seqcore_hooks_t* hooks);

/* アプリのライフサイクルに合わせて初期状態へ戻す。
 * PLAYING 中だった場合は MIDI Stop(0xFC)を送出してから停止する */
void seqcore_reset(void);

/* タイマ発火時にホストが呼ぶ(ディスパッチャ本体) */
void seqcore_dispatch(void);

/* ---- L1: transport(docs/hostapi-next.md §3)---- */
int32_t seqcore_transport_start(void);
int32_t seqcore_transport_stop(void);
int32_t seqcore_transport_continue(void);
int32_t seqcore_transport_locate(uint32_t song_tick);
int32_t seqcore_transport_get_position(void* buf, size_t buf_len);

/* ---- L1: tempomap(同 §4)---- */
int32_t seqcore_tempomap_set_tempo(uint32_t at_song_tick, uint32_t us_per_quarter);
int32_t seqcore_tempomap_set_meter(uint32_t at_song_tick, uint32_t numer, uint32_t denom);
int32_t seqcore_tempomap_set_loop(uint32_t start_song_tick, uint32_t end_song_tick);

/* ---- L0: seq(同 §5)---- */
/* 先頭から連続した n 件(プレフィックス)のみを受理する。残りはアプリが
 * 保持して再送する契約(architecture.md §11-9)*/
int32_t seqcore_seq_write(const void* buf, size_t buf_len);
int32_t seqcore_seq_flush_after(uint32_t tick);
int32_t seqcore_seq_filled_until(void);

/* ---- time(同 §9)---- */
int32_t seqcore_time_us_to_tick(int64_t us);

/* 自己検査(キューの tick 順・安定順序・満杯時の受理数・flush_after の件数)。
 * 失敗件数を返す(0 = PASS)。呼び出すとキューと transport 状態は初期化される */
int seqcore_selftest(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
