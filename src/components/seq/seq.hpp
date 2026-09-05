#pragma once
// L0/L1 の実機アダプタ。Phase 11。
//
// ロジック本体は移植可能な C コア `shared/seq_core.{h,c}` にあり、実機ホストと
// Linux ホストが同一のコードを使う。本ファイルはそのプラットフォーム束ね
//(時刻源 = Clock Authority、排他 = 専用 portMUX、タイマ = esp_timer
// ワンショット、ポート出力 = midi::Midi_TxBytes / トーンパレット)だけを担う。
//
// Host API の 12 関数(hostapi_transport_* 等)は hostapi.cpp が
// `seqcore_*` を直接呼んで実装する。
#include <cstdint>

namespace seq {

// CLICK ポートの発音ハンドラ。トーンパレット(hostapi.cpp のアプリセッション
// 状態)はホスト API 側にあるので、コンポーネント間の循環依存を避けるために
// コールバックで受け取る。呼び出しは esp_timer タスク上・ロック外。
using ClickHandler = void (*)(uint32_t slot);
void SetClickHandler(ClickHandler fn);

// 起動時に 1 回(app_main から。midi::Midi_Init の後)。
void Init();

// アプリのライフサイクルに合わせて初期状態へ戻す(hostapi_audio_reset から)。
void Reset();

#ifdef SEQCORE_SELFTEST
// L0 キューの自己検査(tick 順・安定順序・満杯時の受理数・flush_after の件数)。
// 恒久の opt-in テスト。ビルド時に SEQCORE_SELFTEST を定義すると起動時に走る
// (未定義なら本体ごとリンカに落とされる)。実装は shared/seq_core.c。
void SelfTest();
#endif

} // namespace seq
