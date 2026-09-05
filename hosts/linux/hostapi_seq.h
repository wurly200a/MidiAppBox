/* 音楽時間軸 API(Phase 11)の Linux ホスト実装。
 *
 * L0/L1 のロジックは shared/seq_core.c(実機と**同一のコード**)を使い、
 * ここはプラットフォーム束ね(Clock Authority・排他・ディスパッチスレッド・
 * ポート出力)だけを担う。実機側は src/components/seq/seq.cpp が同じ役割。
 *
 * Clock Authority(docs/architecture.md §3)の Linux 実装:
 *   レートマスターは SDL オーディオコールバックの累計フレーム数で、実機の
 *   I2S on_sent と同型に受け取る。ただし v1 の時刻源は単一時計
 *   (hostapi_midi.c の単調増加 µs = hostapi_midi_recv のタイムスタンプと
 *   同一時基)であり、サンプルカウントはアンカーと ppm 監視に使う。
 *   別クロック系のホスト(ブラウザ = Phase A)では、この ppm を使って比を
 *   補正する実装に差し替える。差し替え点はこのファイルに閉じている。
 */
#pragma once

#include <stdint.h>
#include "wasm_export.h"

/* main スレッドから1回だけ呼ぶ(host_midi_init の後)。 */
void host_seq_init(void);
void host_seq_shutdown(void);

/* アプリのライフサイクルに合わせてリセットする(host_sdl_audio_reset から)。 */
void host_seq_reset(void);

/* SDL オーディオコールバックから、再生済みフレーム数を渡す(レートマスター)。 */
void host_seq_on_audio(uint32_t frames);

/* shared/hostapi_defs.h の X-macro が参照する native_* 実装 */
int32_t native_hostapi_transport_start(wasm_exec_env_t exec_env);
int32_t native_hostapi_transport_stop(wasm_exec_env_t exec_env);
int32_t native_hostapi_transport_continue(wasm_exec_env_t exec_env);
int32_t native_hostapi_transport_locate(wasm_exec_env_t exec_env, int32_t song_tick);
int32_t native_hostapi_transport_get_position(wasm_exec_env_t exec_env, char* buf, uint32_t len);
int32_t native_hostapi_tempomap_set_tempo(wasm_exec_env_t exec_env, int32_t at_tick, int32_t upq);
int32_t native_hostapi_tempomap_set_meter(wasm_exec_env_t exec_env, int32_t at_tick,
                                          int32_t numer, int32_t denom);
int32_t native_hostapi_tempomap_set_loop(wasm_exec_env_t exec_env, int32_t start, int32_t end);
int32_t native_hostapi_seq_write(wasm_exec_env_t exec_env, const char* buf, uint32_t len);
int32_t native_hostapi_seq_flush_after(wasm_exec_env_t exec_env, int32_t tick);
int32_t native_hostapi_seq_filled_until(wasm_exec_env_t exec_env);
int32_t native_hostapi_time_us_to_tick(wasm_exec_env_t exec_env, int64_t us);
