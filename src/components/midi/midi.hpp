#pragma once
// MIDI OUT(Phase 8b)。Phase 8a で疎通確認済みの回路(GPIO18 = UART1 TX,
// 31250bps 8N1, 論理反転)を常設の native host 機能として実装する。
//
// hostapi_midi_send の生バイト送信に加え、既存のクリック/トーン予約
// スケジューラ(hostapi.cpp の tone_schedule_impl)からの通知を受けて
// 24ppqn の MIDI Clock(0xF8)をタイマ駆動で生成する。テンポは新規に
// 保持せず、予約時刻の差分(直前発音時刻→次回予約時刻)から都度導出する
// (shared/hostapi_defs.h の "midi" セクション参照)。
#include <cstddef>
#include <cstdint>

namespace midi {

// UART1 の初期化(起動時に1回だけ呼ぶ。app_main.cpp から audio::Audio_Init()
// と同様のタイミングで呼び出す想定)。
void Midi_Init();

// bytes をそのまま MIDI OUT へ送信する。1..8 バイト。成功 0 / 失敗 -1。
// 単独の Start(0xFA)/Continue(0xFB)/Stop(0xFC) を検出したら、内部で
// 24ppqn クロック生成を開始/停止する。
int32_t Midi_Send(const uint8_t* bytes, size_t len);

// 既存クリックスケジューラ(hostapi.cpp)からの通知。App 側の呼び出しとは
// 無関係にクロック生成が停止中(Midi_Send で Start していない)なら no-op。
//
// - Midi_NotifyBeatScheduled: 新しい予約時刻 target_ms が確定した。
//   直前に受け取った予約時刻(発火有無は問わない)との差分を次回のクロック
//   間隔として staging する(実際に使うのは対応する拍が発音された瞬間)。
//   「直前発音時刻との差分」ではなく「直前に受け取った予約時刻との差分」を
//   使うのがポイント: BPM/拍子変更(rearm())は「拍0を今すぐ」を毎回
//   再予約するため、これは常に既存の予約(未来の時刻)より小さく、
//   自然に差分が負になって無視される。一方その直後に続けて予約される
//   「拍1」(新テンポでの本当の次拍)は、この「拍0=今」を基準にした
//   正しい周期を返す。閾値によるフィルタではなく構造的に誤検出しない。
// - Midi_NotifyBeatFired: 拍が実際に発音された(fired_ms)。staging 済みの
//   間隔でクロックタイマを fired_ms 基準に再同期する(毎拍ドリフト補正)。
void Midi_NotifyBeatScheduled(uint32_t target_ms);
void Midi_NotifyBeatFired(uint32_t fired_ms);

// アプリのライフサイクルに合わせてリセットする(hostapi_audio_reset() から
// 呼ぶ)。クロック生成を強制停止し、staging 状態も破棄する。
// MIDI IN 受信リングバッファ(Phase 9a)も破棄する。
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
