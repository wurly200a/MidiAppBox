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
    lv_label_set_text(label, "MP3 Player\n");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

    // Status label (Now Playing / Stopped)
    status_lbl_ = lv_label_create(scr);
    lv_obj_set_style_text_color(status_lbl_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(status_lbl_, "Stopped");
    lv_obj_align(status_lbl_, LV_ALIGN_TOP_LEFT, 6, 28);

    btn_ok_ = lv_button_create(scr);
    lv_obj_set_size(btn_ok_, 120, 40);
    lv_obj_align(btn_ok_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_ok_, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_add_event_cb(btn_ok_, &Ui::ok_btn_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *btn_lbl = lv_label_create(btn_ok_);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);    
    lv_label_set_text(btn_lbl, "Play");
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
            lv_obj_t* row = lv_button_create(list_cont_);
            lv_obj_remove_style_all(row);
            lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

            lv_obj_t* lbl = lv_label_create(row);
            std::string safe = to_safe_text(name);
            lv_label_set_text(lbl, safe.c_str());
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            // Make row clickable and attach data + handler
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            auto* data = new RowData{this, name};
            lv_obj_add_event_cb(row, &Ui::row_event_cb, LV_EVENT_ALL, data);
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
            lv_obj_t* row = lv_button_create(list_cont_);
            lv_obj_remove_style_all(row);
            lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

            lv_obj_t* lbl = lv_label_create(row);
            std::string safe = to_safe_text(name);
            lv_label_set_text(lbl, safe.c_str());
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            // Make row clickable and attach data + handler
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            auto* data = new RowData{this, name};
            lv_obj_add_event_cb(row, &Ui::row_event_cb, LV_EVENT_ALL, data);
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

void Ui::row_event_cb(lv_event_t* e) {
    auto code = lv_event_get_code(e);
    auto* data = static_cast<RowData*>(lv_event_get_user_data(e));
    if (!data || !data->self) return;
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        data->self->set_row_selected(target, data->name);
    } else if (code == LV_EVENT_DELETE) {
        delete data;
    }
}

void Ui::ok_btn_event_cb(lv_event_t* e) {
    Ui* self = static_cast<Ui*>(lv_event_get_user_data(e));
    if (!self) return;
    if (!self->selected_name_.empty() && self->on_play_request_) {
        self->on_play_request_(self->selected_name_);
    }
}

void Ui::set_row_selected(lv_obj_t* row, const std::string& name) {
    // Clear previous selection visuals
    if (selected_row_) {
        // reset bg
        lv_obj_set_style_bg_opa(selected_row_, LV_OPA_0, 0);
        // set label text color to black
        lv_obj_t* lbl = lv_obj_get_child(selected_row_, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    }
    selected_row_ = row;
    selected_name_ = name;
    if (selected_row_) {
        lv_obj_set_style_bg_color(selected_row_, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_bg_opa(selected_row_, LV_OPA_COVER, 0);
        lv_obj_t* lbl = lv_obj_get_child(selected_row_, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    }
}

void Ui::clear_row_selected() {
    if (selected_row_) {
        lv_obj_set_style_bg_opa(selected_row_, LV_OPA_0, 0);
        lv_obj_t* lbl = lv_obj_get_child(selected_row_, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        selected_row_ = nullptr;
    }
    selected_name_.clear();
}

void Ui::set_play_status(bool playing, const std::string& name_or_hint) {
    // Must be called from LVGL context or guarded by lvgl_port_lock in caller
    if (status_lbl_) {
        if (playing) {
            lv_obj_set_style_text_color(status_lbl_, lv_palette_main(LV_PALETTE_GREEN), 0);
            std::string txt = "Playing: ";
            txt += to_safe_text(name_or_hint);
            lv_label_set_text(status_lbl_, txt.c_str());
        } else {
            lv_obj_set_style_text_color(status_lbl_, lv_palette_main(LV_PALETTE_GREY), 0);
            std::string txt = name_or_hint.empty() ? std::string("Stopped") : name_or_hint;
            lv_label_set_text(status_lbl_, txt.c_str());
        }
    }
    if (btn_ok_) {
        lv_obj_t* btn_lbl = lv_obj_get_child(btn_ok_, 0);
        if (btn_lbl) lv_label_set_text(btn_lbl, playing ? "Stop" : "Play");
    }
}
