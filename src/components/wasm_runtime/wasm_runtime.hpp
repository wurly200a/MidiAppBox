#pragma once

namespace wasmrt {

// Phase 1: フラッシュ埋め込みの hello.wasm をロードして app_init() を呼び、
// 結果と前後の空きヒープをログに出す。成功で true。
bool run_selftest();

// selftest を専用タスク(十分なネイティブスタック)で実行して完了を待つ。
bool run_selftest_task();

// Phase 2: 埋め込み demo.wasm をロードし、app_init() 後に 100ms 周期で
// app_tick() を呼び続けるデモスレッドを起動する(復帰はスレッド起動の成否)。
// 事前に hostapi_display_init() と Audio_Click_Init() を済ませておくこと。
bool run_demo();

} // namespace wasmrt
