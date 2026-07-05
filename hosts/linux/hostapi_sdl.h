/* Linux ホストのホスト API 実装(SDL2 バックエンド)。 */
#pragma once

#include <stdbool.h>
#include "wasm_export.h"

/* SDL の window/renderer/audio を初期化する(main スレッドから) */
bool host_sdl_init(void);
void host_sdl_shutdown(void);

/* retained スロットの内容を 1 フレーム描画する(main ループから毎 tick) */
void host_sdl_render(void);

/* shared/hostapi_defs.h の X-macro が参照する native_* 実装 */
void native_hostapi_draw_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              const char* str, uint32_t len);
void native_hostapi_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t rgb888);
void native_hostapi_play_click(wasm_exec_env_t exec_env);
uint32_t native_hostapi_now_ms(wasm_exec_env_t exec_env);
