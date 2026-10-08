#include "view.hpp"
#include "hud_theme.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace kestrel_ui {
namespace {
lv_color_t color(const float* rgb) { return lv_color_make(uint8_t(rgb[0] * 255), uint8_t(rgb[1] * 255), uint8_t(rgb[2] * 255)); }
lv_opa_t opacity(float v) { return lv_opa_t(std::clamp(v * 255, 0.0f, 255.0f)); }
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, int x, int y) {
    auto* o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_white(), 0);
    lv_obj_set_pos(o, x, y);
    lv_label_set_text(o, "");
    return o;
}
void set_text(lv_obj_t* obj, const std::string& text) {
    if (text != lv_label_get_text(obj)) lv_label_set_text(obj, text.c_str());
}
std::string number(float v, const char* format) { char text[80]; std::snprintf(text, sizeof(text), format, v); return text; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}
void View::Asset::load(const std::string& path, int width, int height) {
    std::ifstream in(path, std::ios::binary);
    uint8_t header[28]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "KUIA", 4) ||
        le32(header + 4) != uint32_t(width) || le32(header + 8) != uint32_t(height))
        throw std::runtime_error("Missing/mismatched canopy asset: " + path + " (run host assetgen for the display resolution)");
    const uint32_t ox = le32(header + 12), oy = le32(header + 16), w = le32(header + 20), h = le32(header + 24);
    if (!w || !h || uint64_t(ox) + w > uint32_t(width) || uint64_t(oy) + h > uint32_t(height))
        throw std::runtime_error("Invalid canopy asset bounds");
    x = int(ox); y = int(oy);
    pixels.resize(size_t(w) * h);
    if (!in.read(reinterpret_cast<char*>(pixels.data()), pixels.size()) || in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Invalid canopy asset length");
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_A8;
    image.header.w = w; image.header.h = h; image.header.stride = w;
    image.data_size = pixels.size(); image.data = pixels.data();
}
View::View(Model& model, int w, int h, const std::string& assets) : model_(model), width_(w), height_(h) {
    assets_[0].load(assets + "/canopy-left.kui", w, h);
    assets_[1].load(assets + "/canopy-right.kui", w, h);
    screen_ = lv_screen_active();
    lv_obj_remove_style_all(screen_);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        panels_[i] = lv_obj_create(screen_);
        lv_obj_remove_style_all(panels_[i]);
        // Includes rails/type above the baked ground, with motion bleed room.
        lv_obj_set_pos(panels_[i], i ? w / 2 : 0, h * 2 / 3);
        lv_obj_set_size(panels_[i], w / 2, h - h * 2 / 3);
        lv_obj_remove_flag(panels_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(panels_[i], draw_event, LV_EVENT_DRAW_MAIN, this);
        auto place = [&](float x, float y, const lv_font_t* font) {
            auto p = canopy::project(w, h, i == 0, x, y);
            // Right-panel text extends left from its anchor. Glyphs remain
            // runtime text; no asset contains live telemetry or state labels.
            auto* o = label(screen_, font, int(p.x), int(p.y) - font->line_height);
            if (i) lv_obj_set_style_translate_x(o, -int(w * 0.18f), 0);
            return o;
        };
        headings_[i] = place(14, 512, &lv_font_montserrat_40);
        states_[i] = place(i ? 40 : 60, 456, &lv_font_montserrat_20);
        for (int j = 0; j < 3; ++j) details_[i][j] = place(14, 551 + j * 17, &lv_font_montserrat_20);
    }
    branding_ = label(screen_, &lv_font_montserrat_28, 32, 24);
    lv_label_set_text(branding_, "KESTREL / ASCENT");
    status_ = label(screen_, &lv_font_montserrat_20, 32, 66);
    menu_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(menu_);
    lv_obj_set_size(menu_, w * 42 / 100, h * 48 / 100);
    lv_obj_set_style_bg_color(menu_, lv_color_hex(0x061c2b), 0);
    lv_obj_set_style_bg_opa(menu_, 232, 0);
    lv_obj_set_style_border_color(menu_, lv_color_hex(0x00e5ff), 0);
    lv_obj_set_style_border_width(menu_, 2, 0);
    lv_obj_set_style_pad_all(menu_, 24, 0);
    lv_obj_remove_flag(menu_, LV_OBJ_FLAG_SCROLLABLE);
    menu_title_ = label(menu_, &lv_font_montserrat_28, 0, 0);
    for (int i = 0; i < 6; ++i) {
        menu_rows_[i] = label(menu_, &lv_font_montserrat_20, 0, 58 + i * 43);
        lv_obj_set_width(menu_rows_[i], w * 42 / 100 - 48);
        lv_label_set_long_mode(menu_rows_[i], LV_LABEL_LONG_DOT);
    }
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
}
View::~View() { lv_obj_clean(screen_); }
void View::draw_event(lv_event_t* e) {
    auto* self = static_cast<View*>(lv_event_get_user_data(e));
    self->draw_panel(lv_event_get_layer(e), lv_event_get_target(e) == self->panels_[0]);
}
void View::draw_panel(lv_layer_t* layer, bool left) {
    const auto& th = hud_theme_at(model_.theme);
    const auto& a = assets_[left ? 0 : 1];
    // Retain instrument furniture while initially waiting for a backend. The
    // acquired/lost animations themselves are shared with the GLES OSD.
    const auto& v = model_.visual;
    const float dim = v.visible ? v.idle_dim : 0.65f;
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &a.image; image.recolor = color(th.ground); image.recolor_opa = LV_OPA_COVER;
    image.opa = opacity(dim * (v.visible ? v.shade : 1));
    lv_area_t area{a.x, a.y, a.x + int(a.image.header.w) - 1, a.y + int(a.image.header.h) - 1};
    lv_draw_image(layer, &image, &area);
    for (float y : {456.0f, 596.0f}) {
        const float reach = 252 * (v.visible ? v.rule : 1);
        for (int segment = 0; segment < 16; ++segment) {
            const float x0 = -canopy::kBleed + (252 + canopy::kBleed) * segment / 16;
            const float x1 = std::min(reach, -canopy::kBleed + (252 + canopy::kBleed) * (segment + 1) / 16);
            if (x1 <= x0) continue;
            const auto p = canopy::project(width_, height_, left, x0, y), q = canopy::project(width_, height_, left, x1, y);
            lv_draw_line_dsc_t line;
            lv_draw_line_dsc_init(&line);
            line.p1 = {int32_t(std::lround(p.x)), int32_t(std::lround(p.y))};
            line.p2 = {int32_t(std::lround(q.x)), int32_t(std::lround(q.y))};
            line.width = std::max(1, height_ / 360);
            line.color = color(model_.telemetry.link_valid(now_) ? th.data : th.quiet);
            const float taper = std::clamp((x0 - 126) / 126, 0.0f, 1.0f);
            line.opa = opacity(0.8f * (1 - taper * taper) * (v.visible ? v.rail_dim : dim));
            lv_draw_line(layer, &line);
        }
    }
    // Segmented MCS track. An unknown value is an unlit track, not a zero
    // presented as a valid measurement. The left FC track stays unlit.
    const int filled = !left && model_.telemetry.link_valid(now_) ? std::clamp(model_.telemetry.mcs + 1, 0, 8) : 0;
    const int count = left ? 20 : 8;
    for (int i = 0; i < count; ++i) {
        const float x0 = 65 + 200.0f * i / count, x1 = x0 + 200.0f / count * 0.75f;
        const float bottom = 539, top = bottom - 9 * (1 - 0.32f * i / count);
        const auto p0 = canopy::project(width_, height_, left, x0, top);
        const auto p1 = canopy::project(width_, height_, left, x1, top);
        const auto p2 = canopy::project(width_, height_, left, x1, bottom);
        const auto p3 = canopy::project(width_, height_, left, x0, bottom);
        for (auto points : {std::array<canopy::Point, 3>{p0, p1, p2}, std::array<canopy::Point, 3>{p0, p2, p3}}) {
            lv_draw_triangle_dsc_t tri;
            lv_draw_triangle_dsc_init(&tri);
            for (int j = 0; j < 3; ++j) tri.p[j] = {int32_t(std::lround(points[j].x)), int32_t(std::lround(points[j].y))};
            tri.bg_color = color(th.data);
            tri.bg_opa = opacity((i < filled ? 0.95f : 0.12f) * dim * (v.visible ? v.block : 1));
            lv_draw_triangle(layer, &tri);
        }
    }
}
void View::update_text(bool appearance_changed) {
    const auto& t = model_.telemetry;
    const bool fresh = t.fresh(now_), valid = t.link_valid(now_);
    set_text(status_, model_.status(now_));
    set_text(headings_[0], "-- V");
    set_text(states_[0], "FC UNKNOWN");
    set_text(details_[0][0], "AIRCRAFT TELEMETRY UNAVAILABLE");
    set_text(details_[0][1], fresh ? number(t.cpu_temperature_x100 / 100.0f, "VRX %.1f C") : "VRX -- C");
    set_text(details_[0][2], fresh && t.voltage_mv > 0 ? number(t.voltage_mv / 1000.0f, "SUPPLY %.2f V") : "SUPPLY -- V");
    set_text(headings_[1], valid ? "MCS " + std::to_string(t.mcs) : "MCS --");
    set_text(states_[1], valid ? "RF LINK" : "IDLE");
    set_text(details_[1][0], "VIDEO PROGRESS UNVERIFIED");
    set_text(details_[1][1], valid ? "FREQ RAW " + std::to_string(t.frequency_raw) : "FREQ RAW --");
    set_text(details_[1][2], fresh ? "RF TEMP RAW " + std::to_string(t.rf_temperature_raw) : "RF TEMP RAW --");
    if (appearance_changed) {
        const auto& th = hud_theme_at(model_.theme);
        lv_obj_set_style_text_color(status_, color(valid ? th.data : th.quiet), 0);
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_text_color(states_[i], color(th.quiet), 0);
            lv_obj_set_style_text_opa(headings_[i], opacity(valid ? 1 : 0.65f), 0);
            for (auto* d : details_[i]) lv_obj_set_style_text_opa(d, opacity(fresh ? 1 : 0.65f), 0);
        }
    }
}
void View::update_menu() {
    set_text(menu_title_, std::string("KESTREL / ") + Model::tab_name(model_.tab));
    const auto rows = model_.rows(now_);
    for (int i = 0; i < 6; ++i) {
        if (i < int(rows.size())) {
            set_text(menu_rows_[i], std::string(i == model_.selected ? "> " : "  ") + rows[i].name + "   " + rows[i].value);
            if (old_selected_ != model_.selected || old_tab_ != model_.tab || old_theme_ != model_.theme)
                lv_obj_set_style_text_color(menu_rows_[i], i == model_.selected ? color(hud_theme_at(model_.theme).data) : lv_color_white(), 0);
        } else set_text(menu_rows_[i], "");
    }
}
void View::update(uint64_t now) {
    now_ = now;
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const float target = model_.menu_open ? 1 : 0;
    const float next = menu_progress_ + std::clamp(target - menu_progress_, -dt / 0.24f, dt / 0.24f);
    if (next != menu_progress_) {
        menu_progress_ = next;
        const float ease = next * next * (3 - 2 * next);
        lv_obj_set_pos(menu_, int(-width_ * 0.43f + width_ * 0.47f * ease), height_ / 8);
        if (next > 0) lv_obj_remove_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
    }
    const auto& t = model_.telemetry;
    const bool fresh = t.fresh(now), valid = t.link_valid(now);
    const bool appearance = !initialized_ || old_theme_ != model_.theme || old_fresh_ != fresh || old_valid_ != valid;
    const bool data = !initialized_ || t.connected != presented_.connected || t.mcs != presented_.mcs ||
        t.frequency_raw != presented_.frequency_raw || t.distance_raw != presented_.distance_raw ||
        t.quality_raw != presented_.quality_raw || t.voltage_mv != presented_.voltage_mv ||
        t.cpu_temperature_x100 != presented_.cpu_temperature_x100 || t.rf_temperature_raw != presented_.rf_temperature_raw;
    const bool menu = old_menu_ != model_.menu_open || old_tab_ != model_.tab || old_selected_ != model_.selected;
    if (appearance || data) update_text(appearance);
    const bool counters = t.status_frames != presented_.status_frames ||
                          t.unknown_frames != presented_.unknown_frames || t.bad_frames != presented_.bad_frames;
    if (appearance || data || menu || (model_.menu_open && model_.tab == 2 && counters)) update_menu();
    // A repeated identical status packet must not force a panel redraw. Local
    // animation is the only continuous producer; freshness still expires even
    // when no new packet arrives.
    if (appearance || t.mcs != presented_.mcs || model_.visual.animating)
        for (auto* p : panels_) lv_obj_invalidate(p);
    presented_ = t; initialized_ = true;
    old_fresh_ = fresh; old_valid_ = valid; old_theme_ = model_.theme;
    old_menu_ = model_.menu_open; old_tab_ = model_.tab; old_selected_ = model_.selected;
}
} // namespace kestrel_ui
