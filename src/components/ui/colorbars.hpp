// ---- colorbars.hpp ----
#pragma once
#include "lvgl.h"

static inline void paint_test_colorbars(lv_obj_t* parent) {
    // Full-screen container
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);  // black background
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);

    // Horizontal layout & equal distribution (via flex grow)
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont,
                          LV_FLEX_ALIGN_START,  // main_place
                          LV_FLEX_ALIGN_START,  // cross_place
                          LV_FLEX_ALIGN_START); // track_cross_place

    const lv_color_t cols[] = {
        lv_color_hex(0xFF0000), // Red
        lv_color_hex(0x00FF00), // Green
        lv_color_hex(0x0000FF), // Blue
        lv_color_hex(0xFFFFFF), // White
        lv_color_hex(0x808080), // Gray
        lv_color_hex(0xFFFF00), // Yellow
        lv_color_hex(0x00FFFF), // Cyan
        lv_color_hex(0xFF00FF), // Magenta
        lv_color_hex(0x000000), // Black
    };
    const int n = sizeof(cols)/sizeof(cols[0]);

    for (int i = 0; i < n; ++i) {
        lv_obj_t* bar = lv_obj_create(cont);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, cols[i], 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_size(bar, LV_SIZE_CONTENT, lv_pct(100)); // height is full
        lv_obj_set_style_flex_grow(bar, 1, 0);              // equal width
    }
}
