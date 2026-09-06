#pragma once
// MIDI OUT(Phase 8b)。Phase 8a で疎通確認済みの回路(GPIO18 = UART1 TX,
// 31250bps 8N1, 論理反転)を常設の native host 機能として実装する。
//
// MIDI Clock(0xF8)の生成はホストの音楽時間軸 API(transport / tempomap、
// shared/seq_core.c / src/components/seq/)が 40 tick グリッドから直接
// 行う(docs/architecture.md §5)。ここでの生バイト送信(Midi_Send /
// Midi_TxBytes)にテンポ逆算・クロック生成の副作用は一切ない(Phase 14 で
// 旧経路を削除。旧仕様は docs/results/phase09c.md / phase14.md 参照)。
#include <cstddef>
#include <cstdint>

namespace midi {

// UART1 の初期化(起動時に1回だけ呼ぶ。app_main.cpp から audio::Audio_Init()
// と同様のタイミングで呼び出す想定)。
void Midi_Init();

// bytes をそのまま MIDI OUT へ送信する。1..8 バイト。成功 0 / 失敗 -1。
// System Realtime の送出(Start/Stop/Continue/Clock を含む)は
// transport_* を使うこと(こちらは生バイトを渡すだけで副作用を持たない)。
int32_t Midi_Send(const uint8_t* bytes, size_t len);

// L0 のポート層(Phase 11)からの生バイト送出。Midi_Send と違い Start/Stop の
// 副作用を持たない(音楽時間軸は L1 が持つため、ここで再解釈しない)。
//
// UART TX は Midi_Send と共通の短い spinlock で直列化する。複数タスクが
// 同時に uart_write_bytes を呼ぶとバイトが交錯しうるため。len は 1..8。
// docs/architecture.md §7 の送出規律により、呼び出し側は 1 回のメッセージを
// 小さく(3〜4 バイト)保ち、大きなバーストは分割して渡すこと。
void Midi_TxBytes(const uint8_t* bytes, size_t len);

// アプリのライフサイクルに合わせてリセットする(hostapi_audio_reset() から
// 呼ぶ)。MIDI IN 受信リングバッファ(Phase 9a)を破棄する
// (MIDI Clock 生成は L1/seq::Reset() 側が止める。Phase 14)。
void Midi_Reset();

// ---- MIDI IN(Phase 9a)----
// UART1 RX(GPIO15, TLP2361 受信回路, Phase 8c で検証済み)。受信バイトを
// UART イベントタスクで受信直後にタイムスタンプ付きでリングバッファへ積み、
// この関数で吸い出す。パースは一切行わない(shared/hostapi_defs.h の
// "midi" セクション参照)。
//
// buf は hostapi_midi_recv_t の配列として書き込む。buf_len はバイト数。
// buf_len / 16 件を上限にリングバッファから吸い出し、書いた件数を返す
// (0 = なし)。
int32_t Midi_Recv(void* buf, size_t buf_len);

} // namespace midi
