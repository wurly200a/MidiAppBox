
#pragma once
#include "lvgl.h"
#include <vector>
#include <string>

class Ui {
public:
    explicit Ui(lv_display_t* disp) : disp_(disp) {}
    void build();
    void show_file_list(const std::vector<std::string>& files);
    // Call this only from LVGL thread (e.g., via lv_async_call)
    void show_file_list_from_lvgl(const std::vector<std::string>& files);
private:
    lv_display_t* disp_ = nullptr;
    lv_obj_t* list_cont_ = nullptr;
};
