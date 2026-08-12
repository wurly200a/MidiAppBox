/* MIDI OUT(Phase 8b)。実機の UART1 MIDI OUT に相当する Linux ホスト実装。
 * ALSA シーケンサ(libasound, 任意)経由で実際の MIDI ポート(例: UM-ONE)へ
 * 送信する。ALSA が使えない環境ではバイト列を stderr にログ出力するだけの
 * ダミー動作にフォールバックする(ビルドは通り、送信自体は失敗しない)。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "wasm_export.h"

/* main スレッドから1回だけ呼ぶ。ALSA が見つからない/接続失敗でも false は
 * 返さない(ログ出力フォールバックで動作を継続する)。 */
bool host_midi_init(void);
void host_midi_shutdown(void);

/* アプリのライフサイクルに合わせてリセットする(host_sdl_audio_reset() から
 * 呼ぶ)。MIDI Clock 生成を強制停止する。 */
void host_midi_reset(void);

/* 既存クリックスケジューラ(hostapi_sdl.c の tone_schedule_impl / audio_callback)
 * からの通知。実機側 midi.hpp の Midi_NotifyBeatScheduled/Fired と同じ契約
 * (「直前に受け取った予約時刻」との差分でテンポを導出するため last_fired は
 * 不要。詳細は実機側 midi.hpp のコメント参照)。 */
void host_midi_notify_beat_scheduled(uint32_t target_ms);
void host_midi_notify_beat_fired(uint32_t fired_ms);

/* shared/hostapi_defs.h の X-macro が参照する native_* 実装 */
int32_t native_hostapi_midi_send(wasm_exec_env_t exec_env, const char* bytes, uint32_t len);
