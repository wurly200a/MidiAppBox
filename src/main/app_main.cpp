// MidiAppBox: WASM アプリランチャー (Phase 6D で一本化)。
// 旧 MP3 デモモード(Kconfig 分岐)は Phase 6D で削除した。同等機能は
// WASM アプリ側の mp3player.wasm が提供する。
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "power_key.hpp"
#include "display.hpp"
#include "touch.hpp"
#include "audio.hpp"
#include "midi.hpp"
#include "phase08b_qy70_debug.hpp" // Phase 8b 追記: QY70 切り分け(一時コード)
#include "wasm_runtime.hpp"
#include "hostapi.hpp"
#include "launcher.hpp"

static const char* TAG = "APP";

extern "C" void app_main()
{
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    PowerKey::Config cfg;
    cfg.key_pin = GPIO_NUM_6;
    cfg.latch_pin = GPIO_NUM_7;
    cfg.hold_ms = 2000;          // Power off on 2-second long press
    cfg.poll_period_ms = 10;     // Poll every 10 ms
    cfg.use_deepsleep_hold = true;

    static PowerKey pwr{cfg};
    pwr.init();
    pwr.start_task();

    ESP_LOGI(TAG, "Boot: WASM launcher");
    static Display disp;
    disp.init();
    disp.start_lvgl();
    static Touch touch;
    touch.init(disp.lvgl_get_disp());

    // hostapi_audio_* 用のフル初期化(esp-audio-player タスク起動、実測 ~47KB)
    const size_t heap_before_audio = esp_get_free_heap_size();
    audio::Audio_Init();
    ESP_LOGI(TAG, "Audio_Init: free heap %u -> %u (delta %d)",
             (unsigned)heap_before_audio, (unsigned)esp_get_free_heap_size(),
             (int)(heap_before_audio - esp_get_free_heap_size()));

    // MIDI OUT の常設初期化(Phase 8a で確認済みの UART1 設定。起動時1回のみ)
    midi::Midi_Init();

    phase08b_qy70_debug_start(); // Phase 8b 追記: QY70 切り分け(一時コード)

    if (!wasmrt::runtime_init()) {
        ESP_LOGE(TAG, "WASM runtime init failed");
    }

    // power_key 短押し = ホームボタン(実行中アプリに停止要求 → メニュー復帰)。
    // コールバックは power_key タスク(小スタック)上なので atomic 操作のみ。
    pwr.set_on_short_press([](void*) { wasmrt::app_request_stop(); }, nullptr);

    // SD 準備+メニュー表示は FATFS 用に十分なスタックを持つタスクで行う
    auto boot_task = [](void*) {
        vTaskDelay(pdMS_TO_TICKS(500)); // SD 安定待ち
        char status[64];
        if (!wasmrt::launcher_prepare_sd(status, sizeof(status))) {
            ESP_LOGE(TAG, "SD prepare failed: %s", status);
        }
#if CONFIG_MIDIBOX_WASM_CYCLE_TEST
        else {
            wasmrt::launcher_run_cycle_test();
        }
#endif
        // 失敗時もメニューは出す(エラー表示付き・空リスト)
        wasmrt::launcher_show(status);
        vTaskDelete(nullptr);
    };
    xTaskCreate(boot_task, "wasm_boot", 8192, nullptr, 4, nullptr);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
