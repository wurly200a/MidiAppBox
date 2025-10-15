#pragma once
#include <cstdint>
#include "lvgl.h"

class Touch {
public:
    // Receive LVGL v9 display handle and register pointer input
    void init(lv_display_t* disp);
};
