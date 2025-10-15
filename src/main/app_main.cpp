#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "power_key.hpp"
#include "display.hpp"
#include "touch.hpp"
#include "ui.hpp"

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

    ESP_LOGI(TAG, "UI ready.");

    // Main loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
