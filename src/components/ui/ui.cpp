#include "ui.hpp"
#include "esp_lvgl_port.h"  // added
#include "colorbars.hpp"
using std::string; using std::vector;

static inline std::string to_safe_text(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const size_t maxlen = 80;
    size_t count = 0;
    for (unsigned char c : in) {
        if (count >= maxlen) break;
        if (c >= 0x20 && c <= 0x7E) out.push_back((char)c);
        else out.push_back('?');
        ++count;
    }
    return out;
}

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

    // Container for file list (empty initially)
    list_cont_ = lv_obj_create(scr);
    lv_obj_remove_style_all(list_cont_);
    lv_obj_set_size(list_cont_, lv_pct(100), lv_pct(60));
    lv_obj_align(list_cont_, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_opa(list_cont_, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(list_cont_, 4, 0);
    lv_obj_set_flex_flow(list_cont_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_cont_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list_cont_, LV_DIR_VER);

    lvgl_port_unlock();
}

void Ui::show_file_list(const vector<string>& files) {
    if (!list_cont_) return;
    lvgl_port_lock(0);
    // Clear previous children safely
    lv_obj_clean(list_cont_);
    if (files.empty()) {
        lv_obj_t* lbl = lv_label_create(list_cont_);
        lv_label_set_text(lbl, "(No files)" );
    } else {
        const size_t max_items = 128;
        size_t shown = 0;
        for (const auto& name : files) {
            if (shown >= max_items) break;
            lv_obj_t* row = lv_obj_create(list_cont_);
            lv_obj_remove_style_all(row);
            lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

            lv_obj_t* lbl = lv_label_create(row);
            std::string safe = to_safe_text(name);
            lv_label_set_text(lbl, safe.c_str());
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            ++shown;
        }
        if (files.size() > max_items) {
            lv_obj_t* more = lv_label_create(list_cont_);
            char buf[48];
            size_t rem = files.size() - max_items;
            lv_snprintf(buf, sizeof(buf), "+%u more...", (unsigned)rem);
            lv_label_set_text(more, buf);
        }
    }
    lvgl_port_unlock();
}

void Ui::show_file_list_from_lvgl(const vector<string>& files) {
    if (!list_cont_) return;
    // Hide container during rebuild to avoid redraw during deletion
    lv_obj_add_flag(list_cont_, LV_OBJ_FLAG_HIDDEN);
    // Clear previous children safely
    lv_obj_clean(list_cont_);
    if (files.empty()) {
        lv_obj_t* lbl = lv_label_create(list_cont_);
        lv_label_set_text(lbl, "(No files)" );
    } else {
        const size_t max_items = 128;
        size_t shown = 0;
        for (const auto& name : files) {
            if (shown >= max_items) break;
            lv_obj_t* row = lv_obj_create(list_cont_);
            lv_obj_remove_style_all(row);
            lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

            lv_obj_t* lbl = lv_label_create(row);
            std::string safe = to_safe_text(name);
            lv_label_set_text(lbl, safe.c_str());
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            ++shown;
        }
        if (files.size() > max_items) {
            lv_obj_t* more = lv_label_create(list_cont_);
            char buf[48];
            size_t rem = files.size() - max_items;
            lv_snprintf(buf, sizeof(buf), "+%u more...", (unsigned)rem);
            lv_label_set_text(more, buf);
        }
    }
    // Show and reset scroll
    lv_obj_clear_flag(list_cont_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(list_cont_, 0, LV_ANIM_OFF);
}
