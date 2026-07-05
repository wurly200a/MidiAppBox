#pragma once

namespace wasmrt {

// Phase 1: フラッシュ埋め込みの hello.wasm をロードして app_init() を呼び、
// 結果と前後の空きヒープをログに出す。成功で true。
bool run_selftest();

// selftest を専用タスク(十分なネイティブスタック)で実行して完了を待つ。
bool run_selftest_task();

} // namespace wasmrt
