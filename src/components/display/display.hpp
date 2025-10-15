
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
