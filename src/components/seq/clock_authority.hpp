#pragma once
// Clock Authority(Phase 11 / 移行ステップ 1)。
// 設計は docs/architecture.md §3。
//
// 系内の全時刻変換を通す唯一のモジュール。(sample_count, host_us) の対応を
// 維持し、「音楽時間軸の現在時刻」を供給する。
//
// 実機のレートマスターは I2S 出力のサンプルカウント(P10-1: on_sent は無音時も
// フリーランするので途切れない)。P10-2 で I2S と esp_timer が同一 XTAL 由来の
// 厳密同比(実効 fs 44100.0000Hz、公称比 -0.00ppm)であることを実証したため、
// 換算は**固定比**でよく、逐次推定(PLL)は不要である。
//
// したがって実機では「音楽時間軸 = esp_timer µs + 固定オフセット」に帰着する。
// サンプルカウント側は
//   (1) アンカーの確立(起動時 / チャネル再構成後)
//   (2) 固定比が本当に保たれているかの常時監視(ppm 推定値の公開)
// に使う。ブラウザホスト等、オーディオクロックとタイマクロックが別系統の
// 環境では EstimatedPpm() を使って比を補正する実装に差し替える
//(差し替え点をこの 1 ファイルに閉じ込めるのが本モジュールの目的)。
//
// チャネル再構成(MP3 の 22.05kHz 等への切替)中は on_sent が発火しないが、
// 音楽時間軸は途切れさせてはならない。実機は同一水晶なので esp_timer による
// 外挿で繋ぐ(§3「レート切替・チャネル再構成時の継続規則」)。この外挿に
// ドリフトは乗らないため、レート切替はアンカーの張り替えに帰着する。
#include <cstddef>
#include <cstdint>

namespace clockauth {

// 起動時に 1 回(audio の I2S 初期化より前に呼んでよい)。
void Init();

// I2S TX の on_sent コールバックから呼ぶ(ISR コンテキスト)。
// bytes = 送出済みバイト数。ログ・確保・ブロッキングは一切行わない。
void OnSent(size_t bytes);

// I2S のフォーマット確定 / 再構成を通知する。再構成の前後どちらで呼んでも
// よいように、フレーム換算のみを更新しアンカーを張り替える。
void OnFormatChanged(uint32_t rate_hz, uint8_t bits, bool stereo);

// 音楽時間軸の現在時刻(µs、単調増加)。L0 ディスパッチャと
// hostapi_time_us_to_tick はこれを唯一の時刻源として使う。
int64_t NowUs();

// hostapi_midi_recv 等、esp_timer 時基で打刻された時刻を音楽時間軸へ移す。
// 実機は恒等(同一時基)。別クロック系のホストではここで比を適用する。
int64_t HostUsFromTimerUs(int64_t timer_us);

// 診断用: 累計フレーム数と、アンカー以降の実測レート偏差(ppm)。
uint64_t Frames();
int32_t EstimatedPpm();

} // namespace clockauth
