// screensaver.cpp — メニュー表示中のスクリーンセーバー / バックライト消灯。
// 設計意図と外部仕様は screensaver.hpp を参照。
#include "screensaver.hpp"

#include "sdkconfig.h"

#if CONFIG_MIDIBOX_SCREENSAVER

#include "display.hpp"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"

#include <atomic>
#include <cstdint>

static const char* TAG = "WASM/SAVER";

namespace {

constexpr uint32_t kSaverMs = (uint32_t)CONFIG_MIDIBOX_SCREENSAVER_SEC * 1000u;
constexpr uint32_t kBlankMs = (uint32_t)CONFIG_MIDIBOX_BACKLIGHT_OFF_SEC * 1000u;
constexpr uint32_t kTickMs  = 200;   // 復帰要求の取りこぼしを防ぐ程度に短く
constexpr int32_t  kDotSize = 14;

enum class State { Active, Saver, Blank };

State       s_state       = State::Active;
lv_obj_t*   s_menu_screen = nullptr;
lv_obj_t*   s_overlay     = nullptr;  // 黒地。復帰タップを食う全画面オブジェクト
lv_obj_t*   s_dot         = nullptr;  // 焼き付き防止でゆっくり動く図形
lv_timer_t* s_timer       = nullptr;

// LVGL タスク以外(タッチイベント CB / 電源キータスク / シリアルコマンド)からの
// 復帰要求。実際の破棄は必ず LVGL タスクの tick 内で行う。
std::atomic<bool> s_wake_req{false};

// 軸ごとに周期の違う往復アニメを掛けて、リサージュ状にゆっくり漂わせる
void start_drift_locked(lv_obj_t* dot, int32_t x_max, int32_t y_max)
{
    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, dot);
    lv_anim_set_exec_cb(&ax, [](void* obj, int32_t v) {
        lv_obj_set_x((lv_obj_t*)obj, v);
    });
    lv_anim_set_values(&ax, 0, x_max);
    lv_anim_set_duration(&ax, 11000);
    lv_anim_set_reverse_duration(&ax, 11000);
    lv_anim_set_repeat_count(&ax, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_start(&ax);

    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, dot);
    lv_anim_set_exec_cb(&ay, [](void* obj, int32_t v) {
        lv_obj_set_y((lv_obj_t*)obj, v);
    });
    lv_anim_set_values(&ay, 0, y_max);
    lv_anim_set_duration(&ay, 8000);
    lv_anim_set_reverse_duration(&ay, 8000);
    lv_anim_set_repeat_count(&ay, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_in_out);
    // 起動のたびに軌跡が変わるよう、片方だけ乱数で遅らせる
    lv_anim_set_delay(&ay, (uint32_t)(esp_random() % 4000u));
    lv_anim_start(&ay);
}

// 復帰タップはここで食い止める(メニューのボタンには届かない)。
// LVGL のイベント CB 内でオブジェクトを消すのは避け、点灯だけ即座に行って
// 実際の破棄は次の tick に回す。
void overlay_pressed_cb(lv_event_t* e)
{
    (void)e;
    display_backlight_set(true);
    s_wake_req.store(true);
}

void enter_saver_locked()
{
    const int32_t w = lv_display_get_horizontal_resolution(nullptr);
    const int32_t h = lv_display_get_vertical_resolution(nullptr);

    s_overlay = lv_obj_create(s_menu_screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, w, h);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_pressed_cb, LV_EVENT_PRESSED, nullptr);

    s_dot = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, kDotSize, kDotSize);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x2a3340), 0);  // 暗めのグレー
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    start_drift_locked(s_dot, w - kDotSize, h - kDotSize);

    s_state = State::Saver;
    ESP_LOGI(TAG, "screensaver on (idle %u s)", (unsigned)(kSaverMs / 1000));
}

void enter_blank_locked()
{
    // 完全な黒にしてから消灯(消灯中は再描画も止める)
    if (s_dot) {
        lv_anim_delete(s_dot, nullptr);
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
    display_backlight_set(false);
    s_state = State::Blank;
    ESP_LOGI(TAG, "backlight off (idle %u s)", (unsigned)(kBlankMs / 1000));
}

void wake_locked()
{
    if (s_overlay) {
        lv_obj_delete(s_overlay);   // 子(ドット)とアニメも一緒に消える
        s_overlay = nullptr;
        s_dot = nullptr;
    }
    if (!display_backlight_is_on()) display_backlight_set(true);
    if (s_state != State::Active) ESP_LOGI(TAG, "wake");
    s_state = State::Active;
    lv_display_trigger_activity(nullptr);
}

void tick_cb(lv_timer_t* t)
{
    (void)t;   // LVGL タスク上 = ロック保持済み

    if (s_wake_req.exchange(false)) wake_locked();

    // メニュー以外(= WASM アプリ実行中)では働かせない
    if (!s_menu_screen || lv_screen_active() != s_menu_screen) {
        if (s_state != State::Active) wake_locked();
        return;
    }

    const uint32_t idle = lv_display_get_inactive_time(nullptr);
    switch (s_state) {
    case State::Active:
        if (idle >= kSaverMs) enter_saver_locked();
        break;
    case State::Saver:
        if (idle < kSaverMs) wake_locked();            // 念のための保険
        else if (idle >= kBlankMs) enter_blank_locked();
        break;
    case State::Blank:
        if (idle < kSaverMs) wake_locked();
        break;
    }
}

} // namespace

namespace wasmrt {

void screensaver_attach(lv_obj_t* menu_screen)
{
    s_menu_screen = menu_screen;
    if (!s_timer) {
        s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
        ESP_LOGI(TAG, "armed: saver %us, backlight off %us",
                 (unsigned)(kSaverMs / 1000), (unsigned)(kBlankMs / 1000));
    }
}

void screensaver_wake_locked() { wake_locked(); }

void screensaver_wake()
{
    lvgl_port_lock(0);
    wake_locked();
    lvgl_port_unlock();
}

void screensaver_request_wake() { s_wake_req.store(true); }

} // namespace wasmrt

#else  // !CONFIG_MIDIBOX_SCREENSAVER

namespace wasmrt {
void screensaver_attach(lv_obj_t*) {}
void screensaver_wake_locked() {}
void screensaver_wake() {}
void screensaver_request_wake() {}
} // namespace wasmrt

#endif
