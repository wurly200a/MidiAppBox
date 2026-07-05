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

} // namespace wasmrt
