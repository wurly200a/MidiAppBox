
#pragma once
#include "lvgl.h"

class Ui {
public:
    explicit Ui(lv_display_t* disp) : disp_(disp) {}
    void build();
private:
    lv_display_t* disp_ = nullptr;
};
