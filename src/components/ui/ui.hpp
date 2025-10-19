
#pragma once
#include "lvgl.h"
#include <vector>
#include <string>
#include <functional>

class Ui {
public:
    explicit Ui(lv_display_t* disp) : disp_(disp) {}
    void build();
    void show_file_list(const std::vector<std::string>& files);
    // Call this only from LVGL thread (e.g., via lv_async_call)
    void show_file_list_from_lvgl(const std::vector<std::string>& files);

    // Set callback invoked when user presses OK (play request)
    void set_on_play_request(std::function<void(const std::string&)> cb) { on_play_request_ = std::move(cb); }
    // Currently selected filename (empty if none)
    const std::string& selected_name() const { return selected_name_; }
private:
    lv_display_t* disp_ = nullptr;
    lv_obj_t* list_cont_ = nullptr;
    lv_obj_t* btn_ok_ = nullptr;
    lv_obj_t* status_lbl_ = nullptr;
    std::function<void(const std::string&)> on_play_request_;
    lv_obj_t* selected_row_ = nullptr;
    std::string selected_name_;

    struct RowData { Ui* self; std::string name; };
    static void row_event_cb(lv_event_t* e);
    static void ok_btn_event_cb(lv_event_t* e);
    void set_row_selected(lv_obj_t* row, const std::string& name);
    void clear_row_selected();

public:
    // Update the status label and OK button label based on playing state
    void set_play_status(bool playing, const std::string& name_or_hint);
};
