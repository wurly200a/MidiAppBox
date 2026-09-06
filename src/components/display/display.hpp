
#pragma once
#include "lvgl.h"

class Display {
public:
    void init();           // SPI + ST7789 + LVGL port
    void start_lvgl();     // Start LVGL timer/task
    lv_display_t* lvgl_get_disp();

private:
    lv_display_t* disp_ = nullptr;
};

// バックライト(PIN_LCD_BL, Active High)の ON/OFF。Display::init 後なら
// どのタスクからでも呼べる(GPIO 出力を叩くだけで LVGL ロックは不要)。
// パネル自体は生かしたままなので、ON に戻せば直前の表示がそのまま出る。
void display_backlight_set(bool on);
bool display_backlight_is_on();
