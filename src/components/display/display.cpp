
#include "display.hpp"
#include "board_pins.hpp"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "color_probe.hpp"

static const char* TAG_DISP = "DISPLAY";

static bool s_backlight_on = false;

// ---- Phase 15 T-2(常設): LVGL のフラッシュ所要時間 ----
//
// 描画バッファが PSRAM に移ったので、フラッシュ 1 回あたりのコストを記録する。
// LV_EVENT_FLUSH_START〜FLUSH_FINISH は **flush コールバックの呼び出し時間**で、
// SPI の DMA 完了までは含まない(完了は on_color_trans_done)。つまりここに出るのは
// キュー投入と、PSRAM バウンスが起きている場合の memcpy のコストである。
// 判定は絶対値ではなく記録が目的。ただし app_tick の最大実行時間が 100ms の
// tick 周期を脅かす水準なら報告して止める(docs/design/phase15-psram.md §3-4)。
namespace {

constexpr int kFlushSamples = 1000;
int64_t  s_flush_start_us = 0;
uint32_t s_flush_us[kFlushSamples];
int      s_flush_n = 0;

void flush_log_stats()
{
    uint64_t sum = 0;
    for (int i = 0; i < kFlushSamples; i++) sum += s_flush_us[i];
    std::sort(s_flush_us, s_flush_us + kFlushSamples);
    ESP_LOGI(TAG_DISP,
             "lvgl flush: min=%u avg=%u p50=%u p95=%u p99=%u max=%u us (n=%d)",
             (unsigned)s_flush_us[0], (unsigned)(sum / kFlushSamples),
             (unsigned)s_flush_us[kFlushSamples / 2],
             (unsigned)s_flush_us[(int)(kFlushSamples * 0.95)],
             (unsigned)s_flush_us[(int)(kFlushSamples * 0.99)],
             (unsigned)s_flush_us[kFlushSamples - 1], kFlushSamples);
}

void on_flush_start(lv_event_t*) { s_flush_start_us = esp_timer_get_time(); }

void on_flush_finish(lv_event_t*)
{
    if (s_flush_start_us == 0) return;
    s_flush_us[s_flush_n++] = (uint32_t)(esp_timer_get_time() - s_flush_start_us);
    s_flush_start_us = 0;
    if (s_flush_n == kFlushSamples) {
        flush_log_stats();
        s_flush_n = 0;
    }
}

} // namespace


void display_backlight_set(bool on)
{
    gpio_set_level(PIN_LCD_BL, on ? 1 : 0);
    s_backlight_on = on;
}

bool display_backlight_is_on() { return s_backlight_on; }

void Display::init() {
    // BL pin
    gpio_config_t io{};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = 1ULL << PIN_LCD_BL;
    gpio_config(&io);
    display_backlight_set(false);

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
    // Phase 15: PSRAM を有効にすると LVGL の描画バッファは MALLOC_CAP_DEFAULT 経由で
    // PSRAM に置かれる。このフラグを立てないと esp_lcd は SPI_TRANS_DMA_USE_PSRAM を
    // 付けず、spi_master が転送のたびに **internal の DMA バッファを一時確保して
    // memcpy する**(最大でフラッシュ 1 回ぶん = 240*40*2 = 19,200 B)。
    // それでは internal を空けた意味がないので直結させる。SPI2_HOST なので
    // 「SPI3 は外部メモリ非対応」の制約にも当たらない。
    // 詳細は docs/design/phase15-psram.md §3-2/§3-3。
    io_config.flags.psram_dma_direct = 1;
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
      .task_max_sleep_ms = 500,
      .task_stack_caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
      .timer_period_ms  = 5,
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

    // Phase 15(常設): 描画バッファが internal と PSRAM のどちらから出たかを記録する。
    // lv_display_get_buf_active() は片面しか返さないので、確保前後の free の差分と
    // 併せて「2 面ぶんが本当に PSRAM から出たか」を確定できるようにしておく。
    constexpr uint32_t kCapsInt = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t int_before   = heap_caps_get_free_size(kCapsInt);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    disp_ = lvgl_port_add_disp(&disp_cfg);
    assert(disp_);

    {
        const size_t int_after   = heap_caps_get_free_size(kCapsInt);
        const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const lv_draw_buf_t* buf = lv_display_get_buf_active(disp_);
        const void* data = buf ? buf->data : nullptr;
        const uintptr_t a = (uintptr_t)data;
        const char* where = (a >= 0x3C000000u && a < 0x3E000000u) ? "PSRAM"
                          : (a >= 0x3FC00000u && a < 0x3FD00000u) ? "internal DRAM"
                                                                  : "?";
        ESP_LOGI(TAG_DISP,
                 "lvgl draw buf: active %p (%s) size %u, cfg %u B x2, "
                 "internal %d B, psram %d B",
                 data, where, (unsigned)(buf ? buf->data_size : 0),
                 (unsigned)(disp_cfg.buffer_size * 2),
                 (int)(int_before - int_after), (int)(psram_before - psram_after));
    }

    lv_display_add_event_cb(disp_, on_flush_start, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(disp_, on_flush_finish, LV_EVENT_FLUSH_FINISH, nullptr);

    // Backlight ON
    display_backlight_set(true);
    ESP_LOGI(TAG_DISP, "Display initialized");
//    probe_color_quadrants(panel_handle, /*H=*/LCD_H_RES, /*V=*/LCD_V_RES);    
}

void Display::start_lvgl() {
    // esp_lvgl_port already created tasks/timers at init; nothing to do here.
}

lv_display_t* Display::lvgl_get_disp() { return disp_; }
