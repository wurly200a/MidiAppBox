// ホスト API v0 実装(実機側)。
//
// 描画モデル: (x,y) をキーにした retained オブジェクト。
// 同じ座標への draw_text / fill_rect は既存の LVGL オブジェクトを更新する。
// スロット数は固定で、あふれたら警告ログを出して無視する(PoC 割り切り)。
#include "hostapi.hpp"
#include "hostapi_defs.h"

#include "wasm_export.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.hpp"
#include "freertos/FreeRTOS.h"

#include <atomic>
#include <cstring>

static const char* TAG = "WASM/API";

namespace {

constexpr int kMaxTextSlots = 16;
constexpr int kMaxRectSlots = 16;
constexpr uint32_t kMaxTextLen = 63;
constexpr int kEventQueueDepth = 16;

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

// ---- 入力イベントキュー (Phase 6A) ----
// 生産者は LVGL タスク(スクリーンの event cb)、消費者は wasm アプリスレッド
// (poll_event)。臨界区間は短い(最大 16 レコードの memcpy)ので spinlock。
hostapi_event_t s_evq[kEventQueueDepth];
int s_evq_head = 0;
int s_evq_count = 0;
bool s_down_delivered = false; // DOWN を配送済みか(孤児 UP の抑止)
portMUX_TYPE s_evq_mux = portMUX_INITIALIZER_UNLOCKED;

void event_queue_reset()
{
    portENTER_CRITICAL(&s_evq_mux);
    s_evq_head = 0;
    s_evq_count = 0;
    s_down_delivered = false;
    portEXIT_CRITICAL(&s_evq_mux);
}

void push_event(uint16_t type, int16_t x, int16_t y)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool dropped = false;

    portENTER_CRITICAL(&s_evq_mux);
    // アプリ起動タップの UP がアプリに漏れないよう、DOWN 未配送の UP は捨てる
    if (type == HOSTAPI_EV_TOUCH_UP && !s_down_delivered) {
        portEXIT_CRITICAL(&s_evq_mux);
        return;
    }
    if (type == HOSTAPI_EV_TOUCH_DOWN) s_down_delivered = true;

    if (s_evq_count == kEventQueueDepth) { // 満杯: 最古を捨てる
        s_evq_head = (s_evq_head + 1) % kEventQueueDepth;
        s_evq_count--;
        dropped = true;
    }
    hostapi_event_t& ev = s_evq[(s_evq_head + s_evq_count) % kEventQueueDepth];
    ev.type = type;
    ev.param = 0;
    ev.x = x;
    ev.y = y;
    ev.time_ms = now;
    s_evq_count++;
    portEXIT_CRITICAL(&s_evq_mux);

    if (dropped) ESP_LOGW(TAG, "event queue full, dropped oldest");
}

// アプリスクリーンの PRESSED/RELEASED(LVGL タスクから)
void screen_input_event_cb(lv_event_t* e)
{
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        push_event(HOSTAPI_EV_TOUCH_DOWN, (int16_t)p.x, (int16_t)p.y);
    } else if (code == LV_EVENT_RELEASED) {
        push_event(HOSTAPI_EV_TOUCH_UP, (int16_t)p.x, (int16_t)p.y);
    }
}

// ---- native implementations (wasm import "env") ----
// 文字列引数はシグネチャ "*~" により WAMR が境界検証済みのネイティブポインタで渡す。

void native_hostapi_draw_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                      const char* str, uint32_t len)
{
    (void)exec_env;

    char buf[kMaxTextLen + 1];
    if (len > kMaxTextLen) len = kMaxTextLen;
    memcpy(buf, str, len);
    buf[len] = '\0';

    lvgl_port_lock(0);
    if (!s_screen) {
        lvgl_port_unlock();
        return;
    }
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

void native_hostapi_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                      int32_t w, int32_t h, uint32_t rgb888)
{
    (void)exec_env;

    lvgl_port_lock(0);
    if (!s_screen) {
        lvgl_port_unlock();
        return;
    }
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
            // タッチをスクリーンに素通しする(イベントキューの捕捉点はスクリーン)
            lv_obj_remove_flag(slot->rect, LV_OBJ_FLAG_CLICKABLE);
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

void native_hostapi_play_click(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    audio::Play_Click();
}

uint32_t native_hostapi_now_ms(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- オーディオ API (Phase 6B) ----
// audio::Mp3Player の薄いラッパ。状態はホスト側で宣言的に管理し、
// 自然終了(finished フラグ)だけ get_state/ctrl 時に取り込む。
constexpr const char* kMusicRoot = "/sdcard/music";

std::atomic<int> s_audio_state{HOSTAPI_AUDIO_STOPPED};

// PLAYING 中に自然終了していたら FINISHED へ(呼び出しスレッドは wasm のみ)
void audio_refresh_finished()
{
    if (s_audio_state.load() == HOSTAPI_AUDIO_PLAYING && audio::Music_finished()) {
        s_audio_state.store(HOSTAPI_AUDIO_FINISHED);
    }
}

// ミュージックルート相対パスの検証(サンドボックス境界)
bool audio_path_ok(const char* path, uint32_t len)
{
    if (len == 0 || len > 64) return false;
    if (path[0] == '/') return false;
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (path[i] == '.' && path[i + 1] == '.') return false;
    }
    return true;
}

int32_t native_hostapi_audio_play(wasm_exec_env_t exec_env, const char* path, uint32_t len)
{
    (void)exec_env;
    char rel[65];
    if (!audio_path_ok(path, len)) {
        ESP_LOGW(TAG, "audio_play: rejected path");
        s_audio_state.store(HOSTAPI_AUDIO_ERROR);
        return -1;
    }
    memcpy(rel, path, len);
    rel[len] = '\0';

    char full[96];
    snprintf(full, sizeof(full), "%s/%s", kMusicRoot, rel);
    if (!audio::Music_play_path(full)) {
        ESP_LOGW(TAG, "audio_play: failed: %s", full);
        s_audio_state.store(HOSTAPI_AUDIO_ERROR);
        return -1;
    }
    ESP_LOGI(TAG, "audio_play: %s", full);
    s_audio_state.store(HOSTAPI_AUDIO_PLAYING);
    return 0;
}

int32_t native_hostapi_audio_ctrl(wasm_exec_env_t exec_env, int32_t cmd)
{
    (void)exec_env;
    audio_refresh_finished();
    const int st = s_audio_state.load();
    switch (cmd) {
    case HOSTAPI_AUDIO_CMD_PAUSE:
        if (st != HOSTAPI_AUDIO_PLAYING) return -1;
        audio::Music_pause();
        s_audio_state.store(HOSTAPI_AUDIO_PAUSED);
        return 0;
    case HOSTAPI_AUDIO_CMD_RESUME:
        if (st != HOSTAPI_AUDIO_PAUSED) return -1;
        audio::Music_resume();
        s_audio_state.store(HOSTAPI_AUDIO_PLAYING);
        return 0;
    case HOSTAPI_AUDIO_CMD_STOP:
        audio::Music_stop();
        s_audio_state.store(HOSTAPI_AUDIO_STOPPED);
        return 0;
    default:
        return -1;
    }
}

void native_hostapi_audio_set_volume(wasm_exec_env_t exec_env, int32_t v)
{
    (void)exec_env;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio::Volume_adjustment((uint8_t)v);
}

int32_t native_hostapi_audio_get_state(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    audio_refresh_finished();
    return s_audio_state.load();
}

// buf は WAMR 境界検証済み(シグネチャ "*~")。書いた件数を返す。
int32_t native_hostapi_poll_event(wasm_exec_env_t exec_env, char* buf, uint32_t len)
{
    (void)exec_env;
    const uint32_t max_events = len / sizeof(hostapi_event_t);
    int32_t n = 0;

    portENTER_CRITICAL(&s_evq_mux);
    while (n < (int32_t)max_events && s_evq_count > 0) {
        memcpy(buf + n * sizeof(hostapi_event_t), &s_evq[s_evq_head],
               sizeof(hostapi_event_t));
        s_evq_head = (s_evq_head + 1) % kEventQueueDepth;
        s_evq_count--;
        n++;
    }
    portEXIT_CRITICAL(&s_evq_mux);
    return n;
}

// 登録テーブルは shared/hostapi_defs.h の X-macro から生成(Linux ホストと共通)
NativeSymbol s_native_symbols[] = {
    HOSTAPI_NATIVE_SYMBOLS(HOSTAPI_SYMBOL_ENTRY)
};

} // namespace

namespace wasmrt {

void hostapi_app_screen_create()
{
    lvgl_port_lock(0);
    if (s_screen) {
        // 前回分が残っていたら作り直す(通常は destroy 済みのはず)
        lv_obj_delete(s_screen);
    }
    for (auto& t : s_texts) t = TextSlot{};
    for (auto& r : s_rects) r = RectSlot{};
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    // アプリ実行中のタッチはこのスクリーンで受けてイベントキューへ流す
    lv_obj_add_event_cb(s_screen, screen_input_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_screen, screen_input_event_cb, LV_EVENT_RELEASED, nullptr);
    lv_screen_load(s_screen);
    lvgl_port_unlock();
    event_queue_reset();
}

void hostapi_app_screen_destroy()
{
    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
        for (auto& t : s_texts) t = TextSlot{};
        for (auto& r : s_rects) r = RectSlot{};
    }
    lvgl_port_unlock();
    event_queue_reset();
}

void hostapi_audio_reset()
{
    // ライフサイクル契約: アプリ破棄時にオーディオを必ず停止する。
    // アプリ起動直前にも呼び、STOPPED 状態から開始させる。
    // 状態変数に頼らず無条件で止める(アイドル時の stop は無害)。
    audio::Music_stop();
    s_audio_state.store(HOSTAPI_AUDIO_STOPPED);
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
