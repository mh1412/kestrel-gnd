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
std::string number(float v, const char* format) { char text[80]; std::snprintf(text, sizeof(text), format, v); return text; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void hidden(lv_obj_t* object, bool hide) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void rect(lv_layer_t* layer, int x, int y, int w, int h, const float* rgb, float alpha, int border = 0) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color(rgb); d.bg_opa = border ? lv_opa_t(0) : opacity(alpha);
    d.border_color = color(rgb); d.border_opa = border ? opacity(alpha) : lv_opa_t(0); d.border_width = border;
    lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &a);
}
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
    image.header.magic = LV_IMAGE_HEADER_MAGIC; image.header.cf = LV_COLOR_FORMAT_A8;
    image.header.w = w; image.header.h = h; image.header.stride = w;
    image.data_size = pixels.size(); image.data = pixels.data();
}
View::View(Model& model, int w, int h, const std::string& assets)
    : model_(model), width_(w), height_(h), text_(assets + "/canopy-font.kuf", w, h) {
    assets_[0].load(assets + "/canopy-left.kui", w, h);
    assets_[1].load(assets + "/canopy-right.kui", w, h);
    screen_ = lv_screen_active();
    lv_obj_remove_style_all(screen_);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        panels_[i] = lv_obj_create(screen_);
        lv_obj_remove_style_all(panels_[i]);
        lv_obj_set_pos(panels_[i], i ? w / 2 : 0, h * 2 / 3);
        lv_obj_set_size(panels_[i], w / 2, h - h * 2 / 3);
        lv_obj_remove_flag(panels_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(panels_[i], draw_event, LV_EVENT_DRAW_MAIN, this);
    }
    status_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(status_);
    lv_obj_set_pos(status_, w / 4, h / 3);
    lv_obj_set_size(status_, w / 2, h / 5);
    lv_obj_remove_flag(status_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(status_, draw_event, LV_EVENT_DRAW_MAIN, this);
    menu_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(menu_);
    lv_obj_set_size(menu_, w * 34 / 100, h * 62 / 100);
    lv_obj_remove_flag(menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
}
View::~View() { lv_obj_clean(screen_); }
void View::draw_event(lv_event_t* e) {
    auto* self = static_cast<View*>(lv_event_get_user_data(e));
    auto* target = lv_event_get_target(e);
    auto* layer = lv_event_get_layer(e);
    if (target == self->menu_) self->draw_menu(layer);
    else if (target == self->status_) self->draw_status(layer);
    else self->draw_panel(layer, target == self->panels_[0]);
}
void View::track(lv_layer_t* layer, bool left, float start, float end, float y,
                 float height, int segments, float filled, const float* rgb, float alpha) {
    const float pitch = (end - start) / (segments * (1 - 0.32f / 2));
    float cursor = start;
    const int shown = std::clamp(int(filled * segments + 0.5f), 0, segments);
    const int revealed = std::clamp(int(model_.visual.block * segments + 0.5f), 0, segments);
    for (int i = 0; i < std::min(segments, revealed); ++i) {
        const float factor = 1 - 0.32f * i / std::max(1, segments - 1);
        const float step = pitch * factor, width = step * 10 / 13;
        const float x0 = cursor, x1 = x0 + width;
        const float top = y - height * factor;
        const auto p0 = canopy::project(width_, height_, left, x0, top);
        const auto p1 = canopy::project(width_, height_, left, x1, top);
        const auto p2 = canopy::project(width_, height_, left, x1, y);
        const auto p3 = canopy::project(width_, height_, left, x0, y);
        for (auto points : {std::array<canopy::Point, 3>{p0, p1, p2}, std::array<canopy::Point, 3>{p0, p2, p3}}) {
            lv_draw_triangle_dsc_t tri;
            lv_draw_triangle_dsc_init(&tri);
            for (int j = 0; j < 3; ++j) tri.p[j] = {int32_t(std::lround(points[j].x)), int32_t(std::lround(points[j].y))};
            tri.bg_color = color(rgb);
            tri.bg_opa = opacity((i < shown ? 0.7f + 0.3f * (1 - float(i) / segments) : 0.12f) * alpha);
            lv_draw_triangle(layer, &tri);
        }
        cursor += step;
    }
}
void View::draw_panel(lv_layer_t* layer, bool left) {
    const auto& v = model_.visual;
    if (!v.visible) return; // Original IDLE_1: panels arrive with the first RF link.
    const auto& th = hud_theme_at(model_.theme);
    const auto& t = model_.telemetry;
    const auto& bf = model_.flight;
    const bool flight_fresh = bf.stamp_us && now_ >= bf.stamp_us && now_ - bf.stamp_us < 2000000;
    const auto& a = assets_[left ? 0 : 1];
    const float dim = v.idle_dim;
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = &a.image; image.recolor = color(th.ground); image.recolor_opa = LV_OPA_COVER;
    image.opa = opacity(dim * v.shade);
    lv_area_t area{a.x, a.y, a.x + int(a.image.header.w) - 1, a.y + int(a.image.header.h) - 1};
    lv_draw_image(layer, &image, &area);

    // State labels sit inside a measured gap, rather than across an unbroken
    // rail. No arm/standby state is invented from receiver-only telemetry.
    const auto f = canopy::screen_frame(width_, height_);
    const float content = dim * v.block * (left && !flight_fresh ? 0.75f : 1.0f);
    auto digits_width = [&](std::string value, float size) {
        for (char& c : value) if (c >= '0' && c <= '9') c = '0';
        return text_.design_advance(value, size);
    };
    auto shared_baseline = [&](float size) { return 512 - (canopy::kTextBig - size) * 0.25f / f.U; };
    float state_x = 60;
    if (left) {
        const bool have_voltage = bf.have_cell_v || bf.have_pack_v;
        const std::string volts = have_voltage ? number(bf.have_cell_v ? bf.cell_v : bf.pack_v, "%.2f") : "--";
        text_.panel(layer, volts, true, 14, 512, canopy::kTextBig, false, have_voltage ? th.text : th.quiet, content);
        float x = 14 + digits_width(volts, canopy::kTextBig);
        if (have_voltage) {
            text_.panel(layer, "V", true, x, shared_baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.text, content * 0.72f);
            x += text_.design_advance("V", canopy::kTextUnit);
        }
        if (bf.have_amps) {
            x += 10 * f.U / f.UX;
            text_.panel(layer, "\xe2\x80\xa2", true, x, shared_baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.quiet, content);
            x += text_.design_advance("\xe2\x80\xa2", canopy::kTextUnit) + 10 * f.U / f.UX;
            state_x = x;
            const auto amps = number(bf.amps, "%.1f");
            text_.panel(layer, amps, true, x, shared_baseline(canopy::kTextUnit), canopy::kTextUnit, false, th.text, content * 0.82f);
            x += digits_width(amps, canopy::kTextUnit);
            text_.panel(layer, "A", true, x, shared_baseline(canopy::kTextSmall), canopy::kTextSmall, false, th.text, content * 0.72f);
        }
    } else if (bf.have_speed) {
        text_.panel(layer, "km/h", false, 14, shared_baseline(canopy::kTextSmall), canopy::kTextSmall, true, th.text, content * 0.72f);
        text_.panel(layer, number(bf.speed_kmh, "%.0f"), false, 14 + text_.design_advance("km/h", canopy::kTextSmall),
                    512, canopy::kTextBig, true, th.text, content);
    } else text_.panel(layer, "--", false, 14, 512, canopy::kTextBig, true, th.quiet, content);
    const bool idle = !left && v.mark > 0;
    std::string state;
    if (idle) { state = "IDLE"; state_x = 150; }
    else if (left && bf.arm == 0) state = "DISARMED";
    else if (left && bf.arm == 1) {
        if (bf.have_timer) { char timer[24]; std::snprintf(timer, sizeof(timer), "%d'%02d\"", bf.timer_s / 60, bf.timer_s % 60); state = timer; }
        else state = "ARMED";
    }
    const float state_width = text_.design_advance(state, canopy::kTextState);
    constexpr float rest[] = {0.663f, 0.706f, 0.741f};
    for (float y : {456.0f, 596.0f}) {
        const float reach = 252 * v.rule;
        for (int segment = 0; segment < 32; ++segment) {
            float x0 = -canopy::kBleed + (252 + canopy::kBleed) * segment / 32;
            float x1 = std::min(reach, -canopy::kBleed + (252 + canopy::kBleed) * (segment + 1) / 32);
            if (x1 <= x0) continue;
            const float gap_a = state_x - 8, gap_b = state_x + state_width + 10;
            std::array<std::array<float, 2>, 2> spans{{{x0, x1}, {0, 0}}};
            if (!state.empty() && y == 456) {
                spans = {{{x0, std::min(x1, gap_a)}, {std::max(x0, gap_b), x1}}};
            }
            for (auto span : spans) {
                if (span[1] <= span[0]) continue;
                const auto p = canopy::project(width_, height_, left, span[0], y), q = canopy::project(width_, height_, left, span[1], y);
                lv_draw_line_dsc_t line;
                lv_draw_line_dsc_init(&line);
                line.p1 = {int32_t(std::lround(p.x)), int32_t(std::lround(p.y))};
                line.p2 = {int32_t(std::lround(q.x)), int32_t(std::lround(q.y))};
                line.width = std::max(1, height_ / 360); line.color = color(left && bf.arm == 1 ? th.accent : rest);
                const float taper = std::clamp((span[0] - 126) / 126, 0.0f, 1.0f);
                line.opa = opacity(0.8f * (1 - taper * taper) * v.rail_dim);
                lv_draw_line(layer, &line);
            }
        }
    }
    // Original two-track layout and labels. No FC flags, no invented readings.
    const float label_width = text_.design_advance(left ? "CELL" : "VIDEO", canopy::kTextLabel);
    const float start = 14 + label_width + 12;
    text_.panel(layer, left ? "CELL" : "RC", left, 14, 527, canopy::kTextLabel, !left, th.text, content);
    text_.panel(layer, left ? "SAG" : "VIDEO", left, 14, 537, canopy::kTextLabel, !left, th.accent, content);
    const float cell_level = bf.have_cell_v ? std::clamp((bf.cell_v - 3.3f) / 0.9f, 0.0f, 1.0f) : 0;
    const float rc = bf.have_lq ? std::clamp(bf.lq_pct / 100.0f, 0.0f, 1.0f) : 0;
    track(layer, left, start, 271, 525, 9, 20, left ? cell_level : rc, th.data, content);
    // Match Kestrel's seven-rung visual ladder; retain the raw MCS in the row.
    constexpr int ladder[] = {1, 2, 5, 7, 8, 10, 12};
    int rung = 0;
    if (!left && t.link_valid(now_)) for (int value : ladder) if (t.mcs >= value) ++rung;
    const float sag = bf.have_cell_v ? std::clamp((cell_peak_ - bf.cell_v) / 0.5f, 0.0f, 1.0f) : 0;
    track(layer, left, start, 271, 534, 4, left ? 20 : 7, left ? sag : float(rung) / 7, th.accent, content);
    if (!state.empty()) {
        text_.panel(layer, state, left, state_x, 456 + canopy::kTextState / f.U / 2,
                    canopy::kTextState, !left, left && bf.arm == 1 ? th.accent : th.quiet, idle ? v.mark : content);
    }
    if (left) {
        const std::string mode = bf.have_mode ? std::string("\xee\x80\x80   ") +
            std::string(bf.mode, std::find(bf.mode, bf.mode + sizeof(bf.mode), '\0')) : "\xee\x80\x80   FC TELEMETRY --";
        text_.panel(layer, mode, true, 14, 560, canopy::kTextRow, false, bf.have_mode ? th.text : th.quiet, content);
        std::string vrx = "\xee\x80\x81   " + (t.fresh(now_) ? number(t.cpu_temperature_x100 / 100.0f, "VRX %.0fC") : "VRX --C");
        if (t.fresh(now_) && t.voltage_mv > 0) vrx += "  \xe2\x80\xa2 " + number(t.voltage_mv / 1000.0f, "%.1fV");
        text_.panel(layer, vrx, true, 14, 584, canopy::kTextRow, false, th.text, content);
    } else {
        const std::string frequency = (t.link_valid(now_) ? "FREQ RAW " + std::to_string(t.frequency_raw) : "FREQ RAW --") + std::string("   \xee\x80\x80");
        text_.panel(layer, frequency, false, 14, 560, canopy::kTextRow, true, th.text, content);
        const std::string mcs = (t.link_valid(now_) ? "VRX  MCS " + std::to_string(t.mcs) : "VRX  MCS --") + std::string("   \xee\x80\x81");
        text_.panel(layer, mcs, false, 14, 584, canopy::kTextRow, true, th.text, content);
    }
}
void View::draw_status(lv_layer_t* layer) {
    const auto& th = hud_theme_at(model_.theme);
    const std::string title = "KESTREL";
    text_.canvas(layer, title, (width_ - text_.advance(title) * 48 / canopy::kFontPixels) / 2,
                 height_ * 0.39f, 48, false, th.text, 0.9f);
    const auto message = model_.status(now_);
    text_.canvas(layer, message, (width_ - text_.advance(message) * 20 / canopy::kFontPixels) / 2,
                 height_ * 0.44f, 20, false, th.quiet, 1);
}
void View::draw_menu(lv_layer_t* layer) {
    const auto& th = hud_theme_at(model_.theme);
    lv_area_t bounds;
    lv_obj_get_coords(menu_, &bounds);
    const int x = bounds.x1, y = bounds.y1, w = lv_obj_get_width(menu_), h = lv_obj_get_height(menu_);
    const float u = height_ / 1080.0f;
    const int pad = int(22 * u), row_h = int(50 * u);
    rect(layer, x, y, w, h, th.ground, 0.94f);
    text_.canvas(layer, model_.tab == 0 ? "AIR UNIT" : "GOGGLE", pad, pad + 14 * u, 15 * u, false, th.accent, 1, x, y);
    int tab_x = pad;
    for (int tab = 0; tab < 3; ++tab) {
        if ((tab == 0) != (model_.tab == 0)) continue;
        const std::string title = Model::tab_name(tab);
        const int tw = int(text_.advance(title) * 21 * u / canopy::kFontPixels);
        text_.canvas(layer, title, tab_x, pad + 52 * u, 21 * u, false,
                     tab == model_.tab ? th.text : th.quiet, 1, x, y);
        if (tab == model_.tab) rect(layer, x + tab_x, y + pad + int(61 * u), tw, std::max(1, int(3 * u)), th.accent, 1);
        tab_x += tw + int(26 * u);
    }
    const int list_top = pad + int(78 * u);
    rect(layer, x + pad, y + list_top - int(6 * u), w - 2 * pad, 1, th.quiet, 0.3f);
    const auto rows = model_.rows(now_);
    for (int i = 0; i < int(rows.size()); ++i) {
        const int ry = list_top + i * row_h;
        const bool selected = i == model_.selected;
        if (selected) {
            rect(layer, x + pad / 2, y + ry + 2, w - pad, row_h - 4, th.ground, 1);
            rect(layer, x + pad / 2, y + ry + 2, w - pad, row_h - 4, th.accent, 1, std::max(1, int(3 * u)));
        }
        const float baseline = ry + row_h * 0.5f + 23 * u * 0.36f;
        text_.canvas(layer, rows[i].name, pad, baseline, 23 * u, false, th.text, selected ? 1 : 0.82f, x, y);
        const float available = w - 2 * pad - text_.advance(rows[i].name) * 23 * u / canopy::kFontPixels - 16 * u;
        const float value_width = text_.advance(rows[i].value) * 20 * u / canopy::kFontPixels;
        const float size = value_width > available && available > 0 ? std::max(12 * u, 20 * u * available / value_width) : 20 * u;
        text_.canvas(layer, rows[i].value, w - pad, baseline, size, true,
                     selected ? th.accent : th.quiet, 1, x, y);
    }
    const int help_y = h - int(110 * u);
    rect(layer, x + pad, y + help_y, w - 2 * pad, 1, th.quiet, 0.35f);
    text_.canvas(layer, "LEFT / RIGHT  section", pad, help_y + 28 * u, 18 * u, false, th.quiet, 0.8f, x, y);
    text_.canvas(layer, "UP / DOWN  select     OK  local theme", pad, help_y + 54 * u, 18 * u, false, th.quiet, 0.8f, x, y);
    text_.canvas(layer, "Stock backend controls not yet enabled", pad, help_y + 80 * u, 18 * u, false, th.quiet, 0.8f, x, y);
}
void View::update(uint64_t now) {
    text_.begin_frame();
    now_ = now;
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const float target = model_.menu_open ? 1 : 0;
    const float next = menu_progress_ + std::clamp(target - menu_progress_, -dt / 0.24f, dt / 0.24f);
    const bool moving = next != menu_progress_;
    menu_progress_ = next;
    const float ease = next * next * (3 - 2 * next);
    const bool right_side = model_.tab == 0;
    const int menu_w = lv_obj_get_width(menu_);
    const int mx = right_side ? width_ - int(menu_w * ease) : int(-menu_w * (1 - ease));
    if (moving || old_tab_ != model_.tab) lv_obj_set_pos(menu_, mx, height_ * 8 / 100);
    hidden(menu_, next <= 0);
    const auto& t = model_.telemetry;
    const auto& flight = model_.flight;
    const bool flight_fresh = flight.stamp_us && now >= flight.stamp_us && now - flight.stamp_us < 2000000;
    if (flight_fresh && flight.have_cell_v) cell_peak_ = std::max(cell_peak_, flight.cell_v);
    const bool fresh = t.fresh(now), valid = t.link_valid(now);
    const bool appearance = !initialized_ || old_theme_ != model_.theme || old_fresh_ != fresh || old_valid_ != valid;
    const bool data = !initialized_ || model_.flight_version != flight_version_ || flight_fresh != old_flight_fresh_ ||
        t.connected != presented_.connected || t.mcs != presented_.mcs ||
        t.frequency_raw != presented_.frequency_raw || t.distance_raw != presented_.distance_raw ||
        t.quality_raw != presented_.quality_raw || t.voltage_mv != presented_.voltage_mv ||
        t.cpu_temperature_x100 != presented_.cpu_temperature_x100 || t.rf_temperature_raw != presented_.rf_temperature_raw;
    const bool menu = old_menu_ != model_.menu_open || old_tab_ != model_.tab || old_selected_ != model_.selected;
    const bool counters = t.status_frames != presented_.status_frames || t.unknown_frames != presented_.unknown_frames || t.bad_frames != presented_.bad_frames;
    if (appearance || data || menu || (model_.menu_open && model_.tab == 2 && counters)) lv_obj_invalidate(menu_);
    if (appearance || data || model_.visual.animating) for (auto* p : panels_) lv_obj_invalidate(p);
    hidden(status_, valid);
    if (!valid) {
        if (appearance || data) lv_obj_invalidate(status_);
    }
    presented_ = t; initialized_ = true;
    old_fresh_ = fresh; old_valid_ = valid; old_theme_ = model_.theme;
    old_menu_ = model_.menu_open; old_tab_ = model_.tab; old_selected_ = model_.selected;
    flight_version_ = model_.flight_version; old_flight_fresh_ = flight_fresh;
}
} // namespace kestrel_ui
