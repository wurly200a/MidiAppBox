// ホスト API v0 実装(実機側)。
//
// 描画モデル: (x,y) をキーにした retained オブジェクト。
// 同じ座標への draw_text / fill_rect は既存の LVGL オブジェクトを更新する。
// スロット数は固定で、あふれたら警告ログを出して無視する(PoC 割り切り)。
#include "hostapi.hpp"

#include "wasm_export.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.hpp"

#include <cstring>

static const char* TAG = "WASM/API";

namespace {

constexpr int kMaxTextSlots = 16;
constexpr int kMaxRectSlots = 16;
constexpr uint32_t kMaxTextLen = 63;

struct TextSlot {
    lv_obj_t* label = nullptr;
    int32_t x = 0, y = 0;
};
struct RectSlot {
    lv_obj_t* rect = nullptr;
    int32_t x = 0, y = 0;
};

lv_obj_t* s_screen = nullptr;
TextSlot s_texts[kMaxTextSlots];
RectSlot s_rects[kMaxRectSlots];

// ---- native implementations (wasm import "env") ----
// 文字列引数はシグネチャ "*~" により WAMR が境界検証済みのネイティブポインタで渡す。

void native_draw_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                      const char* str, uint32_t len)
{
    (void)exec_env;
    if (!s_screen) return;

    char buf[kMaxTextLen + 1];
    if (len > kMaxTextLen) len = kMaxTextLen;
    memcpy(buf, str, len);
    buf[len] = '\0';

    lvgl_port_lock(0);
    TextSlot* slot = nullptr;
    for (auto& t : s_texts) {
        if (t.label && t.x == x && t.y == y) { slot = &t; break; }
    }
    if (!slot) {
        for (auto& t : s_texts) {
            if (!t.label) { slot = &t; break; }
        }
        if (slot) {
            slot->label = lv_label_create(s_screen);
            slot->x = x; slot->y = y;
            lv_obj_set_pos(slot->label, x, y);
            lv_obj_set_style_text_color(slot->label, lv_color_white(), 0);
        }
    }
    if (slot) {
        lv_label_set_text(slot->label, buf);
    } else {
        ESP_LOGW(TAG, "draw_text: no free slot (max %d)", kMaxTextSlots);
    }
    lvgl_port_unlock();
}

void native_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                      int32_t w, int32_t h, uint32_t rgb888)
{
    (void)exec_env;
    if (!s_screen) return;

    lvgl_port_lock(0);
    RectSlot* slot = nullptr;
    for (auto& r : s_rects) {
        if (r.rect && r.x == x && r.y == y) { slot = &r; break; }
    }
    if (!slot) {
        for (auto& r : s_rects) {
            if (!r.rect) { slot = &r; break; }
        }
        if (slot) {
            slot->rect = lv_obj_create(s_screen);
            slot->x = x; slot->y = y;
            lv_obj_remove_style_all(slot->rect); // 枠線・パディングなしの素の矩形
        }
    }
    if (slot) {
        lv_obj_set_pos(slot->rect, x, y);
        lv_obj_set_size(slot->rect, w, h);
        lv_obj_set_style_bg_color(slot->rect, lv_color_hex(rgb888), 0);
        lv_obj_set_style_bg_opa(slot->rect, LV_OPA_COVER, 0);
    } else {
        ESP_LOGW(TAG, "fill_rect: no free slot (max %d)", kMaxRectSlots);
    }
    lvgl_port_unlock();
}

void native_play_click(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    audio::Play_Click();
}

uint32_t native_now_ms(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

NativeSymbol s_native_symbols[] = {
    { "hostapi_draw_text", (void*)native_draw_text, "(ii*~)", nullptr },
    { "hostapi_fill_rect", (void*)native_fill_rect, "(iiiii)", nullptr },
    { "hostapi_play_click", (void*)native_play_click, "()", nullptr },
    { "hostapi_now_ms", (void*)native_now_ms, "()i", nullptr },
};

} // namespace

namespace wasmrt {

void hostapi_display_init()
{
    lvgl_port_lock(0);
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_screen_load(s_screen);
    lvgl_port_unlock();
}

bool hostapi_register_natives()
{
    if (!wasm_runtime_register_natives(
            "env", s_native_symbols,
            sizeof(s_native_symbols) / sizeof(s_native_symbols[0]))) {
        ESP_LOGE(TAG, "wasm_runtime_register_natives failed");
        return false;
    }
    return true;
}

} // namespace wasmrt
