#include "ui.hpp"
#include "esp_lvgl_port.h"  // added
#include "colorbars.hpp"

void Ui::build() {
    lvgl_port_lock(0);

    lv_obj_t* scr = lv_screen_active();

//    paint_test_colorbars(scr);
    
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_label_set_text(label, "hello, world\n");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* btn = lv_button_create(scr);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);    
    lv_label_set_text(btn_lbl, "OK");
    lv_obj_center(btn_lbl);

    lvgl_port_unlock();
}
