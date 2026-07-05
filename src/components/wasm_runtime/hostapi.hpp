#pragma once

namespace wasmrt {

// wasm デモ用の LVGL スクリーンを作成してアクティブにする。
// Display init + LVGL 起動後、run_demo() より前に呼ぶこと。
void hostapi_display_init();

// ホスト API (module "env") を WAMR に登録する。wasm_runtime_full_init 後、
// instantiate より前に呼ぶこと。
bool hostapi_register_natives();

} // namespace wasmrt
