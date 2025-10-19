#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "power_key.hpp"
#include "display.hpp"
#include "touch.hpp"
#include "ui.hpp"
#include "audio.hpp"
#include "sdcard.hpp"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <vector>
#include <string>

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

    Display disp;
    disp.init();
    disp.start_lvgl();

    Touch touch;
    touch.init(disp.lvgl_get_disp());

    Ui ui(disp.lvgl_get_disp());
    ui.build();
    // Initialize audio playback backend
    audio::Audio_Init();

    // Mount SD and list files in a separate task (larger stack, avoid blocking app_main)
    auto sd_task = [](void* arg){
        Ui* pui = static_cast<Ui*>(arg);
        storage::SdCard sd_local;
        auto files_local = new std::vector<std::string>();
        if (sd_local.mount("/sdcard")) {
            *files_local = sd_local.list_dir("/sdcard");
        } else {
            ESP_LOGW(TAG, "SD mount failed");
        }
        struct Ctx { Ui* ui; std::vector<std::string>* files; };
        auto* ctx = new Ctx{pui, files_local};
        auto async_cb = [](void* p){
            auto* c = static_cast<Ctx*>(p);
            c->ui->show_file_list_from_lvgl(*c->files);
            delete c->files;
            delete c;
        };
        lv_async_call(async_cb, ctx);
        vTaskDelete(nullptr);
    };
    // Increase stack to handle FATFS + std::string allocations comfortably
    xTaskCreate(sd_task, "sd_list", 8192, &ui, 4, nullptr);

    // Wire play request (OK button) to audio playback (MP3 only)
    ui.set_on_play_request([&ui](const std::string& name){
        // If already playing, treat OK as stop
        if (audio::Music_is_playing()) {
            audio::Music_stop();
            // reflect in UI
            lvgl_port_lock(0);
            ui.set_play_status(false, "Stopped");
            lvgl_port_unlock();
            return;
        }
        // Else try to start playback for MP3 files
        auto dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = name.substr(dot + 1);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == "mp3") {
                audio::Play_Music("/sdcard", name.c_str());
                // reflect in UI
                lvgl_port_lock(0);
                ui.set_play_status(true, name);
                lvgl_port_unlock();
                return;
            }
        }
        ESP_LOGI(TAG, "Non-MP3 tapped: %s", name.c_str());
    });

    ESP_LOGI(TAG, "UI ready.");

    // Main loop
    bool last_playing = false;
    while (true) {
        bool playing = audio::Music_is_playing();
        if (playing != last_playing || audio::Music_Next_Flag) {
            lvgl_port_lock(0);
            if (playing) {
                ui.set_play_status(true, ui.selected_name());
            } else {
                ui.set_play_status(false, "Stopped");
            }
            lvgl_port_unlock();
            last_playing = playing;
            if (audio::Music_Next_Flag) {
                audio::Music_Next_Flag = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
