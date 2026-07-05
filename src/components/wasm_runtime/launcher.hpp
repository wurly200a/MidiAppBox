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

} // namespace wasmrt
