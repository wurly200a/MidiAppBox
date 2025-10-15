#pragma once
#include "driver/gpio.h"
#include "driver/i2c_types.h"   // i2c_port_t, I2C_NUM_0, etc.

// ===== Waveshare ESP32-S3 Touch LCD 2.8 (ST7789 + CST328) default wiring =====
// LCD (ST7789, SPI)
#define PIN_LCD_MOSI   GPIO_NUM_45
#define PIN_LCD_SCLK   GPIO_NUM_40
#define PIN_LCD_CS     GPIO_NUM_42
#define PIN_LCD_DC     GPIO_NUM_41
#define PIN_LCD_RST    GPIO_NUM_39
#define PIN_LCD_BL     GPIO_NUM_5   // Backlight (Active High)

// Touch (CST328, I2C)
#define PIN_TOUCH_SDA  GPIO_NUM_1
#define PIN_TOUCH_SCL  GPIO_NUM_3
#define PIN_TOUCH_INT  GPIO_NUM_4   // Low active (set -1 if unconnected)
#define PIN_TOUCH_RST  GPIO_NUM_2   // Low reset

// I2C port (new API)
static constexpr i2c_port_t I2C_TOUCH_PORT = I2C_NUM_0;

// ---- Compatibility aliases (to match older naming) ----
static constexpr gpio_num_t LCD_PIN_MOSI = PIN_LCD_MOSI;
static constexpr gpio_num_t LCD_PIN_SCLK = PIN_LCD_SCLK;
static constexpr gpio_num_t LCD_PIN_CS   = PIN_LCD_CS;
static constexpr gpio_num_t LCD_PIN_DC   = PIN_LCD_DC;
static constexpr gpio_num_t LCD_PIN_RST  = PIN_LCD_RST;
static constexpr gpio_num_t LCD_PIN_BL   = PIN_LCD_BL;

// --- Power Key ---
static constexpr gpio_num_t PIN_PWR_KEY   = GPIO_NUM_0;  // e.g., equivalent to BOOT button

// --- LCD parameters ---
static constexpr int LCD_H_RES = 240;
static constexpr int LCD_V_RES = 320;
