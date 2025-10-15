// touch.cpp  (CST328, BASIC: no-rotation/no-mirror, with on-screen debug)

#include "touch.hpp"
#include "board_pins.hpp"

#include <cstring>
#include <cstdint>
#include <algorithm>

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

// ====== Board-dependent pins (can build even if undefined) ======
#ifndef PIN_TOUCH_SDA
#define PIN_TOUCH_SDA GPIO_NUM_1
#endif
#ifndef PIN_TOUCH_SCL
#define PIN_TOUCH_SCL GPIO_NUM_3
#endif
#ifndef PIN_TOUCH_INT
#define PIN_TOUCH_INT GPIO_NUM_4   // If unconnected, use ((gpio_num_t)-1)
#endif
#ifndef PIN_TOUCH_RST
#define PIN_TOUCH_RST GPIO_NUM_2   // If unconnected, use ((gpio_num_t)-1)
#endif
#ifndef I2C_TOUCH_PORT
#define I2C_TOUCH_PORT I2C_NUM_1
#endif

// ====== Rotation/mirroring disabled (BASIC) ======
#ifndef TOUCH_ROT
#define TOUCH_ROT 1
#endif

// ====== Logging control ======
#define TOUCH_FORCE_POLL 1     // Always poll, ignore INT (get it working first)
#define TOUCH_LOG_TAP    1     // Log coordinates on press
#define TOUCH_LOG_HEX    1     // Hex dump first 8 bytes on press
#define TOUCH_LOG_CAL    1     // Log calibration updates
#define TOUCH_LOG_BASIC  1     // Log display coordinates after BASIC transform

// ===== On-screen debug (cursor & coordinate label) =====
#ifdef DEBUG_TOUCH_CURSOR
static lv_obj_t* s_touch_cursor = nullptr;
static lv_obj_t* s_touch_label  = nullptr;
static volatile uint16_t s_last_dx = 0, s_last_dy = 0;
static volatile bool     s_last_pressed = false;
#endif

// ====== CST328 constants ======
static constexpr uint8_t  CST328_ADDR_PRIMARY   = 0x1A;
static constexpr uint8_t  CST328_ADDR_FALLBACK  = 0x15;

// Registers
static constexpr uint16_t REG_READ_XY           = 0xD000;
static constexpr uint16_t REG_READ_NUMBER       = 0xD005;
static constexpr uint16_t REG_INFO_RES_X        = 0xD1F8; // nominal X max
static constexpr uint16_t REG_INFO_RES_Y        = 0xD1FA; // nominal Y max
static constexpr uint16_t REG_DEBUG_INFO_MODE   = 0xD101;
static constexpr uint16_t REG_NORMAL_MODE       = 0xD109;

static const char* TAG = "TOUCH_CST328";

// I2C (new API)
static i2c_master_bus_handle_t s_bus  = nullptr;
static i2c_master_dev_handle_t s_dev  = nullptr;
static uint8_t                  s_addr = 0;
static i2c_port_t               s_port = I2C_TOUCH_PORT;

// Screen size (on the LVGL display side)
static uint16_t s_hres = 0;   // horizontal pixels
static uint16_t s_vres = 0;   // vertical pixels

// TP self-reported maxima (D1F8/D1FA) ... nominal resolution (e.g., 240x320)
static uint16_t s_tp_xmax = 0;
static uint16_t s_tp_ymax = 0;

// === Auto-calibration (learn in raw space) =========================
static bool     s_cal_inited = true;
static uint16_t s_xmin = 1, s_xmax = 239;
static uint16_t s_ymin = 6, s_ymax = 298;

// Normalize raw -> nominal resolution (0..X_MAX-1 / 0..Y_MAX-1)
static inline uint16_t normalize(uint16_t v, uint16_t vmin, uint16_t vmax, uint16_t out_max_minus1)
{
    if (vmax <= vmin + 1) return 0; // not enough samples yet
    const uint32_t span = static_cast<uint32_t>(vmax - vmin);
    uint32_t num = static_cast<uint32_t>(v - vmin) * out_max_minus1;
    uint32_t q = span ? (num / span) : 0;
    if (q > out_max_minus1) q = out_max_minus1;
    return static_cast<uint16_t>(q);
}

//-----------------------------------------------------------------------------
// I2C: read from 16-bit register
//-----------------------------------------------------------------------------
static esp_err_t i2c_read16(uint16_t reg, uint8_t* rx, size_t len, uint32_t timeout_ms = 1000)
{
    if (!s_dev) return ESP_FAIL;
    uint8_t reg_be[2] = { static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, reg_be, sizeof(reg_be), rx, len, static_cast<int>(timeout_ms));
}

//-----------------------------------------------------------------------------
// I2C: write to 16-bit register (if len=0, send only the 2-byte reg)
//-----------------------------------------------------------------------------
static esp_err_t i2c_write16(uint16_t reg, const uint8_t* tx, size_t len, uint32_t timeout_ms = 1000)
{
    if (!s_dev) return ESP_FAIL;
    if (len > 32) return ESP_ERR_INVALID_ARG;

    uint8_t buf[2 + 32];
    buf[0] = static_cast<uint8_t>(reg >> 8);
    buf[1] = static_cast<uint8_t>(reg & 0xFF);
    if (len) std::memcpy(&buf[2], tx, len);

    return i2c_master_transmit(s_dev, buf, 2 + len, static_cast<int>(timeout_ms));
}

//-----------------------------------------------------------------------------
// Switch Debug Info/Normal mode
//-----------------------------------------------------------------------------
static inline void cst328_enter_debug_info_mode() { (void)i2c_write16(REG_DEBUG_INFO_MODE, nullptr, 0, 100); }
static inline void cst328_enter_normal_mode()     { (void)i2c_write16(REG_NORMAL_MODE,     nullptr, 0, 100); }

//-----------------------------------------------------------------------------
// Parse coordinates (CST328: composed from D001/D002/D003. D004=pressure not used)
//   buf[0]=D000, buf[1]=D001(X_hi), buf[2]=D002(Y_hi), buf[3]=D003(nibbles), buf[4]=D004(pressure)
//-----------------------------------------------------------------------------
static inline void parse_xy_CST328_basic(const uint8_t* b, uint16_t& x_raw, uint16_t& y_raw)
{
    // X = D001<<4 | D003[7:4]
    x_raw = (static_cast<uint16_t>(b[1]) << 4) | ((b[3] & 0xF0) >> 4);
    // Y = D002<<4 | D003[3:0]
    y_raw = (static_cast<uint16_t>(b[2]) << 4) |  (b[3] & 0x0F);
}

//-----------------------------------------------------------------------------
// Read 1 point (Number -> XY / fallback) -> return raw
//-----------------------------------------------------------------------------
static bool read_raw_point(uint16_t& x_raw, uint16_t& y_raw, bool& pressed)
{
    // --- Path A: determine by Number register ---
    uint8_t num = 0;
    if (i2c_read16(REG_READ_NUMBER, &num, 1) == ESP_OK) {
        uint8_t touch_cnt = static_cast<uint8_t>(num & 0x0F);
        if (touch_cnt > 0 && touch_cnt <= 5) {
            uint8_t buf[27] = {0};
            if (i2c_read16(REG_READ_XY, buf, sizeof(buf)) != ESP_OK) return false;

            // Check alignment: buf[6] should be 0xAB (D006)
            if (buf[6] != 0xAB) {
                ESP_LOGW(TAG, "WARN: alignment suspect: buf[6]=0x%02X (expected 0xAB).", buf[6]);
            }

            uint8_t zero = 0;
            (void)i2c_write16(REG_READ_NUMBER, &zero, 1, 1000);

#if TOUCH_LOG_HEX
            ESP_LOGI(TAG, "HEX: %02X %02X %02X %02X %02X %02X %02X %02X ...",
                     buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
#endif
            parse_xy_CST328_basic(buf, x_raw, y_raw);
            pressed = true;
#if TOUCH_LOG_TAP
            ESP_LOGI(TAG, "RAW(A): cnt=%u xr=%u yr=%u", touch_cnt, x_raw, y_raw);
#endif
            return true;
        } else {
            uint8_t zero = 0;
            (void)i2c_write16(REG_READ_NUMBER, &zero, 1, 1000);
            pressed = false;
#ifdef DEBUG_TOUCH_CURSOR
            s_last_pressed = false;
#endif
            // Then try fallback path
        }
    }

    // --- Path B: fallback (read only first 8 bytes of D000) ---
    uint8_t b8[8] = {0};
    if (i2c_read16(REG_READ_XY, b8, sizeof(b8)) != ESP_OK) {
        pressed = false;
#ifdef DEBUG_TOUCH_CURSOR
        s_last_pressed = false;
#endif
        return false;
    }

    // Press bit (0x06 means touch)
    bool p = ((b8[0] & 0x0F) == 0x06);
    if (!p) {
        pressed = false;
#ifdef DEBUG_TOUCH_CURSOR
        s_last_pressed = false;
#endif
        return true; // read successfully but not pressed
    }

#if TOUCH_LOG_HEX
    ESP_LOGI(TAG, "HEX(FB): %02X %02X %02X %02X %02X %02X %02X %02X",
             b8[0],b8[1],b8[2],b8[3],b8[4],b8[5],b8[6],b8[7]);
#endif
    // D006 check (b8[6])
    if (b8[6] != 0xAB) {
        ESP_LOGW(TAG, "WARN(FB): alignment suspect: b8[6]=0x%02X (expected 0xAB).", b8[6]);
    }

    parse_xy_CST328_basic(b8, x_raw, y_raw);
    pressed = true;
#if TOUCH_LOG_TAP
    ESP_LOGI(TAG, "RAW(FB A): xr=%u yr=%u", x_raw, y_raw);
#endif
    return true;
}

//-----------------------------------------------------------------------------
// Update calibration (raw space)
//-----------------------------------------------------------------------------
static inline void update_calibration(uint16_t xr, uint16_t yr)
{
    if (!s_cal_inited) {
        s_xmin = xr;
        s_xmax = xr + 1;
        s_ymin = yr;
        s_ymax = yr + 1;
        s_cal_inited = true;
#if TOUCH_LOG_CAL
        ESP_LOGI(TAG, "CAL:init xr=%u yr=%u", xr, yr);
#endif
        return;
    }

    bool changed = false;
    if (xr < s_xmin) { s_xmin = xr; changed = true; }
    if (xr > s_xmax) { s_xmax = xr; changed = true; }
    if (yr < s_ymin) { s_ymin = yr; changed = true; }
    if (yr > s_ymax) { s_ymax = yr; changed = true; }

#if TOUCH_LOG_CAL
    if (changed) {
        ESP_LOGI(TAG, "CAL: x[%u..%u] y[%u..%u]", s_xmin, s_xmax, s_ymin, s_ymax);
    }
#endif
}

// BASIC: apply rotation to normalized (nx, ny), then scale to display
// TOUCH_ROT: 0=0 deg, 1=90 deg CW, 2=180 deg, 3=270 deg CW
static inline void map_basic_to_display(uint16_t& x, uint16_t& y)
{
    if (s_tp_xmax == 0 || s_tp_ymax == 0 || s_hres == 0 || s_vres == 0) return;

    const uint16_t nx = x;
    const uint16_t ny = y;

    // --- Rotation ---
    uint32_t rx=0, ry=0;     // coordinates after rotation
    uint16_t rw=0, rh=0;     // width/height after rotation

    if (TOUCH_ROT == 0) {            // 0 deg
        rx = nx;                     ry = ny;
        rw = s_tp_xmax;              rh = s_tp_ymax;
    } else if (TOUCH_ROT == 1) {     // 90 deg CW (portrait -> landscape)
        rx = ny;                     ry = (s_tp_xmax - 1) - nx;
        rw = s_tp_ymax;              rh = s_tp_xmax;
    } else if (TOUCH_ROT == 2) {     // 180 deg
        rx = (s_tp_xmax - 1) - nx;   ry = (s_tp_ymax - 1) - ny;
        rw = s_tp_xmax;              rh = s_tp_ymax;
    } else {                         // 270 deg CW (=90 deg CCW)
        rx = (s_tp_ymax - 1) - ny;   ry = nx;
        rw = s_tp_ymax;              rh = s_tp_xmax;
    }

    // --- Scale to display resolution ---
    uint32_t dx = rx;
    uint32_t dy = ry;
    if (rw > 1 && s_hres > 1 && rw != s_hres) dx = (rx * (s_hres - 1)) / (rw - 1);
    if (rh > 1 && s_vres > 1 && rh != s_vres) dy = (ry * (s_vres - 1)) / (rh - 1);

    // Clip
    if (dx >= s_hres) dx = s_hres - 1;
    if (dy >= s_vres) dy = s_vres - 1;

    x = static_cast<uint16_t>(dx);
    y = static_cast<uint16_t>(dy);
}

//-----------------------------------------------------------------------------
// LVGL input read callback
//-----------------------------------------------------------------------------
static void indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;
    if (!s_dev) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

#if !TOUCH_FORCE_POLL
    // Read only when INT pin is low (if connected); otherwise always try
    if (PIN_TOUCH_INT != (gpio_num_t)-1) {
        if (gpio_get_level(PIN_TOUCH_INT) != 0) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
    }
#endif

    uint16_t xr=0, yr=0; bool pressed=false;
    if (!read_raw_point(xr, yr, pressed)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
#ifdef DEBUG_TOUCH_CURSOR
        s_last_pressed = false;
#endif
        return;
    }

    // Update learned calibration
    update_calibration(xr, yr);

    // Normalize raw -> nominal (0..X_MAX-1 / 0..Y_MAX-1)
    const uint16_t nx_max = (s_tp_xmax ? s_tp_xmax - 1 : (s_hres ? s_hres - 1 : 239));
    const uint16_t ny_max = (s_tp_ymax ? s_tp_ymax - 1 : (s_vres ? s_vres - 1 : 319));
    uint16_t nx = normalize(xr, s_xmin, s_xmax, nx_max);
    uint16_t ny = normalize(yr, s_ymin, s_ymax, ny_max);

    // Scale directly (rotation mapping handled in map_basic_to_display)
    uint16_t dx = nx, dy = ny;
    map_basic_to_display(dx, dy);

    data->point.x = static_cast<lv_coord_t>(dx);
    data->point.y = static_cast<lv_coord_t>(dy);
    data->state   = LV_INDEV_STATE_PRESSED;

#ifdef DEBUG_TOUCH_CURSOR
    s_last_dx = dx;
    s_last_dy = dy;
    s_last_pressed = true;
#endif

#if TOUCH_LOG_TAP
    ESP_LOGI(TAG, "NORM: xr=%u yr=%u -> nx=%u ny=%u", xr, yr, nx, ny);
#endif
#if TOUCH_LOG_BASIC
    ESP_LOGI(TAG, "BASIC: xr=%u yr=%u -> P(%u,%u)  [XMAX=%u YMAX=%u]",
             xr, yr, dx, dy, s_tp_xmax, s_tp_ymax);
#endif
}

//-----------------------------------------------------------------------------
// Create I2C bus
//-----------------------------------------------------------------------------
static esp_err_t create_bus(i2c_port_t port)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = nullptr; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = nullptr; }

    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port          = port;
    bus_cfg.sda_io_num        = (gpio_num_t)PIN_TOUCH_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)PIN_TOUCH_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority     = 0;
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = true;

    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

//-----------------------------------------------------------------------------
// Add device at the specified address and check communication (100 kHz)
//-----------------------------------------------------------------------------
static esp_err_t attach_device(uint8_t addr, int scl_hz)
{
    i2c_device_config_t dev_cfg{};
    dev_cfg.device_address  = addr;
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz    = scl_hz;

    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t e = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
    if (e != ESP_OK) return e;

    i2c_master_dev_handle_t prev_dev = s_dev;
    s_dev = dev;

    // Read self-reported resolution in Debug Info mode
    cst328_enter_debug_info_mode();
    uint8_t tmp2[2] = {0};
    if (i2c_read16(REG_INFO_RES_X, tmp2, 2, 200) == ESP_OK) {
        s_addr    = addr;
        s_tp_xmax = static_cast<uint16_t>((tmp2[1] << 8) | tmp2[0]);
        if (i2c_read16(REG_INFO_RES_Y, tmp2, 2, 200) == ESP_OK) {
            s_tp_ymax = static_cast<uint16_t>((tmp2[1] << 8) | tmp2[0]);
        }
        cst328_enter_normal_mode();
        vTaskDelay(pdMS_TO_TICKS(10));   // wait for mode switch to stabilize
        return ESP_OK;
    }

    // Restore previous device
    s_dev = prev_dev;
    (void)i2c_master_bus_rm_device(dev);
    return ESP_FAIL;
}

//-----------------------------------------------------------------------------
// Quick scan 0x10-0x1F (for debugging)
//-----------------------------------------------------------------------------
static void scan_addrs(int scl_hz)
{
    ESP_LOGI(TAG, "I2C quick scan (port=%d, %dkHz):", (int)s_port, scl_hz/1000);
    for (uint8_t a = 0x10; a <= 0x1F; ++a) {
        i2c_device_config_t dev_cfg{};
        dev_cfg.device_address  = a;
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz    = scl_hz;

        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(s_bus, &dev_cfg, &dev) != ESP_OK) continue;

        uint8_t dummy = 0;
        esp_err_t r = i2c_master_receive(dev, &dummy, 1, 50);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  - found addr 0x%02X (responded to receive)", a);
        }
        (void)i2c_master_bus_rm_device(dev);
    }
}

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------
void Touch::init(lv_display_t* disp)
{
    s_hres = lv_display_get_horizontal_resolution(disp);
    s_vres = lv_display_get_vertical_resolution(disp);

    // ---- Initialize GPIO (RST/INT) ----
    if (PIN_TOUCH_INT != (gpio_num_t)-1) {
        gpio_config_t io_int{};
        io_int.pin_bit_mask = 1ULL << PIN_TOUCH_INT;
        io_int.mode         = GPIO_MODE_INPUT;
        io_int.pull_up_en   = GPIO_PULLUP_ENABLE;
        io_int.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_int.intr_type    = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_int));
    }
    if (PIN_TOUCH_RST != (gpio_num_t)-1) {
        gpio_config_t io_rst{};
        io_rst.pin_bit_mask = 1ULL << PIN_TOUCH_RST;
        io_rst.mode         = GPIO_MODE_OUTPUT;
        io_rst.pull_up_en   = GPIO_PULLUP_DISABLE;
        io_rst.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_rst.intr_type    = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_rst));

        // Active low: Low -> 10 ms -> High -> 100 ms
        gpio_set_level(PIN_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // ---- I2C bus (scan at 100 kHz) ----
    s_port = I2C_TOUCH_PORT;
    ESP_ERROR_CHECK(create_bus(s_port));
    scan_addrs(100000);

    // Try attaching in order 0x1A -> 0x15
    esp_err_t err = attach_device(CST328_ADDR_PRIMARY, 100000);
    if (err != ESP_OK) err = attach_device(CST328_ADDR_FALLBACK, 100000);

    // If that fails, switch port and retry
    if (err != ESP_OK) {
        s_port = I2C_NUM_0;
        ESP_LOGW(TAG, "Retry on I2C_NUM_0...");
        ESP_ERROR_CHECK(create_bus(s_port));
        scan_addrs(100000);

        err = attach_device(CST328_ADDR_PRIMARY, 100000);
        if (err != ESP_OK) err = attach_device(CST328_ADDR_FALLBACK, 100000);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST328 not responding at 0x1A/0x15 on port%d. Continue without touch.", (int)s_port);
    } else {
        ESP_LOGI(TAG, "Touch online port=%d addr=0x%02X, X_MAX=%u, Y_MAX=%u",
                 (int)s_port, s_addr, s_tp_xmax, s_tp_ymax);
        cst328_enter_normal_mode();
        vTaskDelay(pdMS_TO_TICKS(10)); // wait for Normal mode
    }

    // ---- Register LVGL input device (once) ----
    lvgl_port_lock(0);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, disp);   // bind to display

#ifdef DEBUG_TOUCH_CURSOR
    // ===== Debug cursor =====
    s_touch_cursor = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_touch_cursor, 12, 12);
    lv_obj_set_style_radius(s_touch_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_touch_cursor, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_touch_cursor, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(s_touch_cursor, 0, 0);
    lv_obj_add_flag(s_touch_cursor, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_touch_cursor, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s_touch_cursor, LV_OBJ_FLAG_CLICKABLE);

    // Set cursor to input device (auto-follow touch position)
    lv_indev_set_cursor(indev, s_touch_cursor);

    // ===== Coordinate label =====
    s_touch_label = lv_label_create(lv_screen_active());
    lv_label_set_text(s_touch_label, "x:--- y:---");
    lv_obj_set_style_text_color(s_touch_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_touch_label, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(s_touch_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_pad_all(s_touch_label, 4, 0);
    lv_obj_align(s_touch_label, LV_ALIGN_TOP_LEFT, 4, 4);

    // Timer to update the label every 200 ms
    auto label_timer_cb = [](lv_timer_t* t){
        (void)t;
        static bool last_vis = false;
        if(s_last_pressed){
            if(!last_vis){
                lv_obj_clear_flag(s_touch_label, LV_OBJ_FLAG_HIDDEN);
                last_vis = true;
            }
            char buf[48];
            lv_snprintf(buf, sizeof(buf), "x:%u y:%u", (unsigned)s_last_dx, (unsigned)s_last_dy);
            lv_label_set_text(s_touch_label, buf);
        }else{
            if(last_vis){
                lv_obj_add_flag(s_touch_label, LV_OBJ_FLAG_HIDDEN);
                last_vis = false;
            }
        }
    };
    lv_timer_create(label_timer_cb, 200, nullptr);
#endif
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Touch initialized (CST328)");
}
