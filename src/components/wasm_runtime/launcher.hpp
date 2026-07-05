#pragma once
#include <cstddef>

namespace wasmrt {

// アプリ配置ディレクトリ
constexpr const char* kAppsDir = "/sdcard/apps";

// SD をマウントし kAppsDir を用意する。ディレクトリが無ければ作成し、
// 埋め込みのサンプルアプリ(.wasm)を書き込む(初回セットアップ)。
// 失敗時は false を返し status にメッセージを入れる。
// FATFS を使うため十分なスタック(8KB 以上)のタスクから呼ぶこと。
bool launcher_prepare_sd(char* status, size_t status_len);

// メニュー画面を(初回は作成して)表示する。kAppsDir を再スキャンして
// .wasm の一覧を出す。status_msg は状態行に表示(nullptr なら変更しない)。
// lvgl_port_lock を取るのでどのタスクからでも呼べる。
void launcher_show(const char* status_msg);

// wasmrt::app_start に渡す停止コールバック。メニューへ復帰する。
void launcher_on_app_stopped(const char* error);

// 5C 検証用: demo.wasm の起動→停止を 10 サイクル回して free heap をログし、
// 壊れた .wasm のロードエラー処理も確認する(CONFIG_MIDIBOX_WASM_CYCLE_TEST)。
// launcher_prepare_sd 成功後、十分なスタックのタスクから呼ぶこと。
void launcher_run_cycle_test();

} // namespace wasmrt
