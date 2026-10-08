#pragma once
#include "model.hpp"
#include "lvgl.h"
#include <string>
#include <vector>

namespace kestrel_ui {
class View {
public:
    View(Model& model, int width, int height, const std::string& assets);
    ~View();
    void update(uint64_t now);
private:
    struct Asset {
        std::vector<uint8_t> pixels;
        lv_image_dsc_t image{};
        int x = 0, y = 0;
        void load(const std::string& path, int width, int height);
    };
    Model& model_;
    int width_, height_;
    uint64_t now_ = 0;
    Telemetry presented_;
    bool initialized_ = false, old_fresh_ = false, old_valid_ = false;
    int old_theme_ = -1, old_selected_ = -1, old_tab_ = -1;
    bool old_menu_ = false;
    float menu_progress_ = 0;
    uint64_t last_tick_ = 0;
    lv_obj_t *screen_ = nullptr, *panels_[2]{}, *status_ = nullptr, *branding_ = nullptr, *menu_ = nullptr;
    lv_obj_t *headings_[2]{}, *states_[2]{}, *details_[2][3]{}, *menu_title_ = nullptr, *menu_rows_[6]{};
    Asset assets_[2];
    void draw_panel(lv_layer_t* layer, bool left);
    void update_text(bool appearance_changed);
    void update_menu();
    static void draw_event(lv_event_t* event);
};
} // namespace kestrel_ui
