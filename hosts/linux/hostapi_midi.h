/* MIDI OUT(Phase 8b)/ MIDI IN(Phase 9a)。実機の UART1 MIDI OUT/IN に
 * 相当する Linux ホスト実装。ALSA シーケンサ(libasound, 任意)経由で実際の
 * MIDI ポート(例: UM-ONE)と送受信する。ALSA が使えない環境では、送信は
 * バイト列を stderr にログ出力するだけのダミー動作、受信は常に 0 件を
 * 返すスタブにフォールバックする(ビルドは通り、送受信自体は失敗しない)。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "wasm_export.h"

/* main スレッドから1回だけ呼ぶ。ALSA が見つからない/接続失敗でも false は
 * 返さない(ログ出力フォールバックで動作を継続する)。 */
bool host_midi_init(void);
void host_midi_shutdown(void);

/* アプリのライフサイクルに合わせてリセットする(host_sdl_audio_reset() から
 * 呼ぶ)。MIDI Clock 生成を強制停止し、MIDI IN 受信リングバッファ
 * (Phase 9a)も破棄する。 */
void host_midi_reset(void);

/* 既存クリックスケジューラ(hostapi_sdl.c の tone_schedule_impl / audio_callback)
 * からの通知。実機側 midi.hpp の Midi_NotifyBeatScheduled/Fired と同じ契約
 * (「直前に受け取った予約時刻」との差分でテンポを導出するため last_fired は
 * 不要。詳細は実機側 midi.hpp のコメント参照)。 */
void host_midi_notify_beat_scheduled(uint32_t target_ms);
void host_midi_notify_beat_fired(uint32_t fired_ms);

/* 起動基準の単調増加 µs クロック(実機の esp_timer_get_time() 相当)。
 * hostapi_midi_recv のタイムスタンプと同一時基であり、Phase 11 の
 * Clock Authority(hostapi_seq.c)もこれを時刻源に使う。 */
uint64_t host_midi_now_us(void);

/* L0 のポート層(Phase 11)からの生バイト送出。native_hostapi_midi_send と違い
 * Start/Stop の副作用を持たない(音楽時間軸は L1 が持つため再解釈しない)。 */
void host_midi_tx_bytes(const uint8_t* bytes, size_t len);

/* shared/hostapi_defs.h の X-macro が参照する native_* 実装 */
int32_t native_hostapi_midi_send(wasm_exec_env_t exec_env, const char* bytes, uint32_t len);
int32_t native_hostapi_midi_recv(wasm_exec_env_t exec_env, char* buf, uint32_t len);
