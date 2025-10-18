
#include "display.hpp"
#include "board_pins.hpp"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "color_probe.hpp"

static const char* TAG_DISP = "DISPLAY";

void Display::init() {
    // BL pin
    gpio_config_t io{};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = 1ULL << PIN_LCD_BL;
    gpio_config(&io);
    gpio_set_level(PIN_LCD_BL, 0);

    // SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.miso_io_num = -1;
    buscfg.max_transfer_sz = LCD_H_RES * 80 * 2; // rough estimate
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Panel IO (SPI)
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.pclk_hz = 40 * 1000 * 1000; // adjust if needed
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
//    io_config.spi_mode = 0;
//    io_config.pclk_hz = 26 * 1000 * 1000;
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

    // Panel (assuming ST7789)
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.color_space = ESP_LCD_COLOR_SPACE_RGB;
    panel_config.bits_per_pixel = 16;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true); // ST7789 characteristic
    esp_lcd_panel_mirror(panel_handle, false, true); // adjust orientation
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // Connect LVGL via esp_lvgl_port
    const lvgl_port_cfg_t lvgl_cfg = {
      .task_priority    = 4,
      .task_stack       = 8192,
      .task_affinity    = -1,
      .timer_period_ms  = 5,
      // .task_max_sleep_ms = 10,
      // .task_stack_caps  = 0,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.hres          = LCD_V_RES;
    disp_cfg.vres          = LCD_H_RES;
    disp_cfg.buffer_size   = LCD_H_RES * 40;
    disp_cfg.double_buffer = true;
    disp_cfg.monochrome    = false;
    disp_cfg.rotation = {
      .swap_xy  = true,
      .mirror_x = true,
      .mirror_y = false,
    };
    disp_cfg.color_format  = LV_COLOR_FORMAT_RGB565;

    // Required to avoid assertion failures
    disp_cfg.io_handle     = io_handle;     // added
    disp_cfg.panel_handle  = panel_handle;  // unchanged

    disp_ = lvgl_port_add_disp(&disp_cfg);
    assert(disp_);

    // Backlight ON
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI(TAG_DISP, "Display initialized");
//    probe_color_quadrants(panel_handle, /*H=*/LCD_H_RES, /*V=*/LCD_V_RES);    
}

void Display::start_lvgl() {
    // esp_lvgl_port already created tasks/timers at init; nothing to do here.
}

lv_display_t* Display::lvgl_get_disp() { return disp_; }
