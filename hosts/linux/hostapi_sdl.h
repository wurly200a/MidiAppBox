/* Linux ホストのホスト API 実装(SDL2 バックエンド)。 */
#pragma once

#include <stdbool.h>
#include "wasm_export.h"

/* SDL の window/renderer/audio を初期化する(main スレッドから) */
bool host_sdl_init(void);
void host_sdl_shutdown(void);

/* retained スロットの内容を 1 フレーム描画する(main ループから毎 tick) */
void host_sdl_render(void);

/* アプリの retained スロットを全消去する(アプリ切り替え時。実機の
 * アプリスクリーン再生成に相当) */
void host_sdl_clear_slots(void);

/* 入力イベントキュー (Phase 6A)。main ループがマウスイベントを push し、
 * アプリが hostapi_poll_event で drain する。アプリ切り替え時に clear */
void host_sdl_push_touch(bool down, int x, int y);
void host_sdl_clear_events(void);

/* オーディオ停止+状態リセット (Phase 6B ライフサイクル契約)。
 * アプリ起動直前と破棄時に呼ぶ */
void host_sdl_audio_reset(void);

/* 直描画ヘルパ(ランチャーメニュー用)。begin_frame → rect/text → present */
void host_sdl_begin_frame(uint32_t rgb888);
void host_sdl_rect(int x, int y, int w, int h, uint32_t rgb888);
void host_sdl_text(int x, int y, const char* s, uint32_t rgb888);
void host_sdl_present(void);

/* ウィンドウ座標 → 論理座標(320x240)変換(マウスイベント用) */
void host_sdl_window_to_logical(int wx, int wy, int* lx, int* ly);

/* 座標変換の診断情報を stdout に出す(デバッグ用) */
void host_sdl_debug_dump_coords(int wx, int wy, int lx, int ly);

/* shared/hostapi_defs.h の X-macro が参照する native_* 実装 */
void native_hostapi_draw_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              const char* str, uint32_t len);
void native_hostapi_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t rgb888);
void native_hostapi_play_click(wasm_exec_env_t exec_env);
uint32_t native_hostapi_now_ms(wasm_exec_env_t exec_env);
int32_t native_hostapi_poll_event(wasm_exec_env_t exec_env, char* buf, uint32_t len);
int32_t native_hostapi_audio_play(wasm_exec_env_t exec_env, const char* path, uint32_t len);
int32_t native_hostapi_audio_ctrl(wasm_exec_env_t exec_env, int32_t cmd);
void native_hostapi_audio_set_volume(wasm_exec_env_t exec_env, int32_t v);
int32_t native_hostapi_audio_get_state(wasm_exec_env_t exec_env);
int32_t native_hostapi_fs_list(wasm_exec_env_t exec_env, int32_t idx,
                               char* buf, uint32_t buf_len);
int32_t native_hostapi_click_schedule(wasm_exec_env_t exec_env, int32_t time_ms);
int32_t native_hostapi_tone_define(wasm_exec_env_t exec_env, int32_t slot,
                                   int32_t wave, int32_t freq_hz, int32_t dur_ms,
                                   int32_t level);
int32_t native_hostapi_tone_play(wasm_exec_env_t exec_env, int32_t slot);
int32_t native_hostapi_tone_schedule(wasm_exec_env_t exec_env, int32_t slot,
                                     int32_t time_ms);
