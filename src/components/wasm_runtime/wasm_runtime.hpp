#pragma once

namespace wasmrt {

// ---- Phase 1/4 の単体テスト・計測(スタンドアロン、runtime_init とは併用しない) ----

// フラッシュ埋め込みの hello.wasm をロードして app_init() を呼び、
// 結果と前後の空きヒープをログに出す。成功で true。
bool run_selftest();

// selftest を専用タスク(十分なネイティブスタック)で実行して完了を待つ。
bool run_selftest_task();

// ---- Phase 5: ランタイム常駐+アプリライフサイクル(ホスト所有) ----

// 起動時に一度: WAMR full_init + ホスト API 登録。以後 destroy しない。
bool runtime_init();

// 計測用 bench.wasm(埋め込み)を実行して結果をログする。runtime_init 後に呼ぶ。
void run_bench();

// アプリ停止時コールバック。error は正常停止なら nullptr、異常なら静的文字列。
// アプリ実行スレッドから呼ばれる(LVGL を触るなら lv_async_call 経由にすること)。
using AppStoppedCb = void (*)(const char* error);

// SD 上の .wasm を専用スレッドでロード・実行する。
// app_init() → 100ms 周期で app_tick() → app_request_stop() で停止、
// (export されていれば)app_exit() を呼んでから破棄する。
// 成功=スレッド起動で true(ロード失敗等は on_stopped(error) で通知)。
bool app_start(const char* path, AppStoppedCb on_stopped);

// 実行中アプリに停止を要求する(非同期。停止完了は on_stopped で通知)。
void app_request_stop();

bool app_is_running();

} // namespace wasmrt
