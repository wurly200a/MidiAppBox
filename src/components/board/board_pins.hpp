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

// --- MIDI OUT (自作回路, Phase 8a で疎通確認済み) ---
// UART1 TX。2SC1815 トランジスタドライブ、31250bps 8N1、論理反転あり
// (uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV) が必須)。
static constexpr gpio_num_t PIN_MIDI_TX = GPIO_NUM_18;

// --- MIDI IN (TLP2361 フォトカプラ受信回路, Phase 8c で検証済み) ---
// UART1 RX。DIN5 → 220Ω → TLP2361(1N4148 逆並列)→ VO を直結。
// TLP2361 はトーテムポール出力かつ反転型で、MIDI のカレントループ論理
// (アイドル = 無電流 = 出力 H)がそのまま UART の論理レベルに一致するため、
// RXD 側の反転(UART_SIGNAL_RXD_INV)は不要(TXD 側のみ反転させる)。
static constexpr gpio_num_t PIN_MIDI_RX = GPIO_NUM_15;

// --- LCD parameters ---
static constexpr int LCD_H_RES = 240;
static constexpr int LCD_V_RES = 320;

// ===== SD Card (SPI) default wiring =====
// Define these according to your board if different.
#ifndef PIN_SD_MOSI
#define PIN_SD_MOSI  GPIO_NUM_17
#endif
#ifndef PIN_SD_MISO
#define PIN_SD_MISO  GPIO_NUM_16
#endif
#ifndef PIN_SD_SCLK
#define PIN_SD_SCLK  GPIO_NUM_14
#endif
#ifndef PIN_SD_CS
#define PIN_SD_CS    GPIO_NUM_21
#endif

// SPI host for SD (use SPI3 as LCD uses SPI2)
#ifndef SD_SPI_HOST
#define SD_SPI_HOST SPI3_HOST
#endif

// ===== SD Card (SDMMC) optional wiring =====
// These can be provided via sdkconfig (CONFIG_EXAMPLE_PIN_*) or fallback defaults.
#ifndef PIN_SDMMC_CLK
#ifdef CONFIG_EXAMPLE_PIN_CLK
#define PIN_SDMMC_CLK ((gpio_num_t)CONFIG_EXAMPLE_PIN_CLK)
#else
#define PIN_SDMMC_CLK GPIO_NUM_14
#endif
#endif
#ifndef PIN_SDMMC_CMD
#ifdef CONFIG_EXAMPLE_PIN_CMD
#define PIN_SDMMC_CMD ((gpio_num_t)CONFIG_EXAMPLE_PIN_CMD)
#else
#define PIN_SDMMC_CMD GPIO_NUM_17
#endif
#endif
#ifndef PIN_SDMMC_D0
#ifdef CONFIG_EXAMPLE_PIN_D0
#define PIN_SDMMC_D0 ((gpio_num_t)CONFIG_EXAMPLE_PIN_D0)
#else
#define PIN_SDMMC_D0 GPIO_NUM_16
#endif
#endif

// Optional extra data lines for 4-bit mode; set to -1 if unused
#ifndef PIN_SDMMC_D1
#ifdef CONFIG_EXAMPLE_PIN_D1
#define PIN_SDMMC_D1 CONFIG_EXAMPLE_PIN_D1
#else
#define PIN_SDMMC_D1 -1
#endif
#endif
#ifndef PIN_SDMMC_D2
#ifdef CONFIG_EXAMPLE_PIN_D2
#define PIN_SDMMC_D2 CONFIG_EXAMPLE_PIN_D2
#else
#define PIN_SDMMC_D2 -1
#endif
#endif
#ifndef PIN_SDMMC_D3
#ifdef CONFIG_EXAMPLE_PIN_D3
#define PIN_SDMMC_D3 CONFIG_EXAMPLE_PIN_D3
#elif defined(CONFIG_SD_Card_D3)
#define PIN_SDMMC_D3 CONFIG_SD_Card_D3
#else
#define PIN_SDMMC_D3 -1
#endif
#endif
