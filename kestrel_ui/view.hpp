#pragma once
#include "model.hpp"
#include "perspective_text.hpp"
#include "lvgl.h"
#include <string>
#include <vector>

namespace kestrel_ui {
class View {
public:
    View(Model& model, int width, int height, const std::string& assets);
    ~View();
    void update(uint64_t now);
    size_t text_cache_bytes() const { return text_.cached_bytes(); }
    uint64_t text_rasterizations() const { return text_.rasterizations(); }
    bool animating() const { return animating_; }
    uint64_t menu_snapshots() const { return menu_snapshot_count_; }
private:
    struct Asset {
        std::vector<uint8_t> pixels;
        lv_image_dsc_t image{};
        int x = 0, y = 0;
        void load(const std::string& path, int width, int height);
    };
    Model& model_;
    int width_, height_;
    PerspectiveText text_;
    uint64_t now_ = 0;
    Telemetry presented_;
    bool initialized_ = false, old_fresh_ = false, old_valid_ = false;
    bool old_flight_fresh_ = false;
    uint64_t flight_version_ = 0;
    float cell_peak_ = 0;
    int old_theme_ = -1, old_selected_ = -1, old_tab_ = -1;
    uint64_t menu_version_ = 0;
    uint64_t rejected_inputs_ = 0;
    bool old_details_ = true;
    int active_menu_side_ = 0;
    bool old_menu_ = false;
    float menu_progress_ = 0;
    bool animating_ = false;
    bool capturing_menu_ = false;
    lv_draw_buf_t* menu_snapshot_ = nullptr;
    std::string menu_snapshot_signature_;
    uint64_t menu_snapshot_count_ = 0;
    uint64_t menu_snapshot_checked_us_ = 0;
    uint64_t last_tick_ = 0;
    lv_obj_t *screen_ = nullptr, *panels_[2]{}, *status_ = nullptr, *menu_ = nullptr, *picker_ = nullptr;
    Asset assets_[2];
    void draw_panel(lv_layer_t* layer, bool left);
    void draw_menu(lv_layer_t* layer);
    lv_area_t menu_reveal(float progress) const;
    void draw_status(lv_layer_t* layer);
    void draw_picker(lv_layer_t* layer);
    void prepare_menu_snapshot();
    void drop_menu_snapshot();
    void track(lv_layer_t* layer, bool left, float start, float end, float y,
               float height, int segments, float filled, const float* rgb, float alpha);
    static void draw_event(lv_event_t* event);
};
} // namespace kestrel_ui
