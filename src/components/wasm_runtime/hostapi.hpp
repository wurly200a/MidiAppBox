#pragma once

namespace wasmrt {

// ホスト API (module "env") を WAMR に登録する。wasm_runtime_full_init 後、
// instantiate より前に呼ぶこと(runtime_init() が呼ぶ)。
bool hostapi_register_natives();

// アプリ用の LVGL スクリーンを新規作成してアクティブにする(スロットも初期化)。
// アプリ起動直前に呼ぶ。LVGL タスクまたは lvgl_port_lock 下から。
void hostapi_app_screen_create();

// アプリ用スクリーンを破棄する。先に別スクリーン(メニュー)をロードしてから
// 呼ぶこと(アクティブなスクリーンは削除できないため)。
void hostapi_app_screen_destroy();

// オーディオを停止し状態を STOPPED に戻す(Phase 6B ライフサイクル契約)。
// アプリ起動直前と破棄時に wasm_runtime が呼ぶ。
void hostapi_audio_reset();

} // namespace wasmrt
