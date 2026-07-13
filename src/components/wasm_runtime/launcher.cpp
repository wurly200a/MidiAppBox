// WASM アプリランチャー: SD 準備(5A)+メニュー UI とタッチ起動(5B)。
#include "launcher.hpp"
#include "wasm_runtime.hpp"
#include "hostapi.hpp"

#include "sdcard.hpp"
#include "audio.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "WASM/LAUNCH";

// 初回セットアップ用の埋め込みサンプルアプリ
extern const uint8_t demo_wasm_start[] asm("_binary_demo_wasm_start");
extern const uint8_t demo_wasm_end[]   asm("_binary_demo_wasm_end");
extern const uint8_t bars_wasm_start[] asm("_binary_bars_wasm_start");
extern const uint8_t bars_wasm_end[]   asm("_binary_bars_wasm_end");
extern const uint8_t touch_demo_wasm_start[] asm("_binary_touch_demo_wasm_start");
extern const uint8_t touch_demo_wasm_end[]   asm("_binary_touch_demo_wasm_end");
extern const uint8_t mp3player_wasm_start[] asm("_binary_mp3player_wasm_start");
extern const uint8_t mp3player_wasm_end[]   asm("_binary_mp3player_wasm_end");
extern const uint8_t clicktest_wasm_start[] asm("_binary_clicktest_wasm_start");
extern const uint8_t clicktest_wasm_end[]   asm("_binary_clicktest_wasm_end");
extern const uint8_t metronome_wasm_start[] asm("_binary_metronome_wasm_start");
extern const uint8_t metronome_wasm_end[]   asm("_binary_metronome_wasm_end");
// 検証用 MP3(hostapi_audio_* のミュージックルートへシード)
extern const uint8_t test_mp3_start[] asm("_binary_test_mp3_start");
extern const uint8_t test_mp3_end[]   asm("_binary_test_mp3_end");
extern const uint8_t tune_down_mp3_start[] asm("_binary_tune_down_mp3_start");
extern const uint8_t tune_down_mp3_end[]   asm("_binary_tune_down_mp3_end");
extern const uint8_t tune_duo_mp3_start[] asm("_binary_tune_duo_mp3_start");
extern const uint8_t tune_duo_mp3_end[]   asm("_binary_tune_duo_mp3_end");

namespace {

storage::SdCard s_sd;
bool s_mounted = false;

lv_obj_t* s_menu_screen = nullptr;
lv_obj_t* s_list_cont = nullptr;
lv_obj_t* s_status_lbl = nullptr;

// path が無い、または内容が埋め込みバイナリと異なれば書き込む
// (サンプルアプリの更新をファーム更新に追従させる。同名以外のファイルには触れない)
void seed_file(const char* path, const uint8_t* start, const uint8_t* end)
{
    const size_t size = (size_t)(end - start);

    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == size) {
        // サイズ一致なら内容もチャンク比較(埋め込み MP3 は ~100KB あるため)
        FILE* rf = fopen(path, "rb");
        if (rf) {
            static uint8_t buf[2048];
            bool same = true;
            size_t off = 0;
            while (same && off < size) {
                const size_t chunk = (size - off < sizeof(buf)) ? size - off : sizeof(buf);
                same = fread(buf, 1, chunk, rf) == chunk &&
                       memcmp(buf, start + off, chunk) == 0;
                off += chunk;
            }
            fclose(rf);
            if (same) return;
        }
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "seed: cannot create %s", path);
        return;
    }
    const size_t written = fwrite(start, 1, size, f);
    fclose(f);
    ESP_LOGI(TAG, "seed: wrote %s (%u/%u bytes)", path, (unsigned)written,
             (unsigned)size);
}

bool has_wasm_ext(const char* name)
{
    const size_t len = strlen(name);
    if (len < 6) return false;
    const char* ext = name + len - 5;
    return strcasecmp(ext, ".wasm") == 0;
}

// メニューの行タップ → アプリ起動(LVGL タスクから呼ばれる)
void row_event_cb(lv_event_t* e)
{
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* label = lv_obj_get_child(row, 0);
    const char* name = lv_label_get_text(label);

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", wasmrt::kAppsDir, name);
    ESP_LOGI(TAG, "launch: %s", path);

    wasmrt::hostapi_app_screen_create();
    if (!wasmrt::app_start(path, wasmrt::launcher_on_app_stopped)) {
        // 起動できなければメニューへ戻す
        lv_screen_load(s_menu_screen);
        wasmrt::hostapi_app_screen_destroy();
        lv_label_set_text(s_status_lbl, "busy: another app still stopping");
    }
}

// lvgl_port_lock 下で呼ぶこと
void create_menu_locked()
{
    // 画面はランドスケープ 320x240(display.cpp: hres=LCD_V_RES, swap_xy)
    s_menu_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_menu_screen, lv_color_hex(0x101418), 0);

    lv_obj_t* title = lv_label_create(s_menu_screen);
    lv_label_set_text(title, "WASM Apps (/sdcard/apps)");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_pos(title, 10, 8);

    s_list_cont = lv_obj_create(s_menu_screen);
    lv_obj_set_size(s_list_cont, 300, 164);
    lv_obj_set_pos(s_list_cont, 10, 32);
    lv_obj_set_style_bg_color(s_list_cont, lv_color_hex(0x181e24), 0);
    lv_obj_set_style_border_width(s_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_list_cont, 6, 0);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_cont, 6, 0);

    s_status_lbl = lv_label_create(s_menu_screen);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x90a0b0), 0);
    lv_obj_set_pos(s_status_lbl, 10, 204);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_lbl, 300);
}

// lvgl_port_lock 下で呼ぶこと。kAppsDir を読み直して行を作る。
void rebuild_list_locked()
{
    lv_obj_clean(s_list_cont);

    DIR* dir = opendir(wasmrt::kAppsDir);
    if (!dir) {
        lv_label_set_text(s_status_lbl, "apps dir not found (SD?)");
        return;
    }
    int count = 0;
    while (dirent* ent = readdir(dir)) {
        if (!has_wasm_ext(ent->d_name)) continue;

        lv_obj_t* row = lv_button_create(s_list_cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a3340), 0);
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, ent->d_name);
        lv_obj_center(label);
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, nullptr);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        lv_label_set_text(s_status_lbl, "no .wasm files in /sdcard/apps");
    }
    ESP_LOGI(TAG, "menu: %d app(s) listed", count);
}

} // namespace

namespace wasmrt {

bool launcher_prepare_sd(char* status, size_t status_len)
{
    if (!s_mounted) {
        // 起動直後はカードの準備ができておらずタイムアウトすることがあるため
        // 少し待ってからリトライする
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(400));
            if (s_sd.mount("/sdcard")) {
                s_mounted = true;
                break;
            }
            ESP_LOGW(TAG, "SD mount attempt %d failed", attempt + 1);
        }
        if (!s_mounted) {
            snprintf(status, status_len, "SD mount failed");
            return false;
        }
    }

    struct stat st;
    if (stat(kAppsDir, &st) != 0) {
        if (mkdir(kAppsDir, 0775) != 0) {
            snprintf(status, status_len, "cannot create %s", kAppsDir);
            return false;
        }
        ESP_LOGI(TAG, "created %s", kAppsDir);
    }

    // 初回セットアップ: サンプルアプリを配置
    char path[64];
    snprintf(path, sizeof(path), "%s/demo.wasm", kAppsDir);
    seed_file(path, demo_wasm_start, demo_wasm_end);
    snprintf(path, sizeof(path), "%s/bars.wasm", kAppsDir);
    seed_file(path, bars_wasm_start, bars_wasm_end);
    snprintf(path, sizeof(path), "%s/touch_demo.wasm", kAppsDir);
    seed_file(path, touch_demo_wasm_start, touch_demo_wasm_end);
    snprintf(path, sizeof(path), "%s/mp3player.wasm", kAppsDir);
    seed_file(path, mp3player_wasm_start, mp3player_wasm_end);
    snprintf(path, sizeof(path), "%s/clicktest.wasm", kAppsDir);
    seed_file(path, clicktest_wasm_start, clicktest_wasm_end);
    snprintf(path, sizeof(path), "%s/metronome.wasm", kAppsDir);
    seed_file(path, metronome_wasm_start, metronome_wasm_end);

    // hostapi_audio_* のミュージックルートと検証用 MP3 (Phase 6B)
    if (stat("/sdcard/music", &st) != 0 && mkdir("/sdcard/music", 0775) != 0) {
        ESP_LOGW(TAG, "cannot create /sdcard/music");
    } else {
        seed_file("/sdcard/music/test.mp3", test_mp3_start, test_mp3_end);
        seed_file("/sdcard/music/tune_down.mp3", tune_down_mp3_start, tune_down_mp3_end);
        seed_file("/sdcard/music/tune_duo.mp3", tune_duo_mp3_start, tune_duo_mp3_end);
    }

    snprintf(status, status_len, "SD ready");
    return true;
}

void launcher_show(const char* status_msg)
{
    lvgl_port_lock(0);
    if (!s_menu_screen) create_menu_locked();
    rebuild_list_locked();
    // 呼び出し元のメッセージを優先(再スキャンの汎用メッセージより後に設定)
    if (status_msg) lv_label_set_text(s_status_lbl, status_msg);
    lv_screen_load(s_menu_screen);
    lvgl_port_unlock();
}

void launcher_on_app_stopped(const char* error)
{
    ESP_LOGI(TAG, "app stopped: %s", error ? error : "ok");
    // アプリ実行スレッドから呼ばれる。lock を取って直接 UI を更新する
    // (アクティブスクリーンをメニューに切り替えてからアプリ画面を破棄)。
    char msg[120];
    snprintf(msg, sizeof(msg), "%s%s", error ? "error: " : "",
             error ? error : "app stopped");
    launcher_show(msg);
    hostapi_app_screen_destroy();
}

void launcher_run_cycle_test()
{
    // Phase 6B: WAMR + esp-audio-player 同時搭載でのリーク検証。
    // mp3player.wasm を起動し、「MP3 再生中のままアプリ停止」を毎サイクル行う
    // (破棄時にホストがオーディオを止める契約の検証を兼ねる)。
    char path[96];
    snprintf(path, sizeof(path), "%s/mp3player.wasm", kAppsDir);

    ESP_LOGI(TAG, "=== cycle test start: free heap %u largest %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    for (int i = 1; i <= 10; i++) {
        hostapi_app_screen_create();
        if (!app_start(path, launcher_on_app_stopped)) {
            ESP_LOGE(TAG, "cycle %d: app_start failed", i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        // アプリがタップで行う再生をホスト側から模擬(テスト専用の直接呼び出し)
        audio::Music_play_path("/sdcard/music/test.mp3");
        vTaskDelay(pdMS_TO_TICKS(1500)); // 再生中のまま停止させる
        app_request_stop();
        while (app_is_running()) vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "=== cycle %d done: free heap %u largest %u (playing=%d)", i,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (int)audio::Music_is_playing());
    }

    // 壊れた .wasm がクラッシュせずエラー表示になることを確認
    char bad[96];
    snprintf(bad, sizeof(bad), "%s/broken.wasm", kAppsDir);
    FILE* f = fopen(bad, "wb");
    if (f) {
        static const char garbage[] = "this is not a wasm module at all";
        fwrite(garbage, 1, sizeof(garbage), f);
        fclose(f);
        hostapi_app_screen_create();
        if (app_start(bad, launcher_on_app_stopped)) {
            while (app_is_running()) vTaskDelay(pdMS_TO_TICKS(50));
        }
        remove(bad);
        ESP_LOGI(TAG, "=== broken.wasm test done: free heap %u",
                 (unsigned)esp_get_free_heap_size());
    }

    ESP_LOGI(TAG, "=== cycle test end");
}

} // namespace wasmrt
