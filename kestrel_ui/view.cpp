#include "view.hpp"
#include "hud_theme.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sstream>

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
View::View(const Model& model, int w, int h, const std::string& assets)
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
    picker_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(picker_);
    lv_obj_set_pos(picker_, 0, h * 58 / 100);
    // Only the two label rows live here. Hiding a full-height picker used to
    // invalidate/redraw both complete HUD wings at every open/fold keyframe.
    lv_obj_set_size(picker_, w, h * 74 / 100 - h * 58 / 100);
    lv_obj_remove_flag(picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(picker_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(picker_, LV_OBJ_FLAG_HIDDEN);
    menu_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(menu_);
    lv_obj_set_size(menu_, w * 34 / 100, h * 92 / 100);
    lv_obj_remove_flag(menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_, draw_event, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
}
View::~View() { lv_obj_clean(screen_); drop_menu_snapshot(); }
void View::drop_menu_snapshot() {
    if (!menu_snapshot_) return;
    lv_image_cache_drop(menu_snapshot_); lv_image_header_cache_drop(menu_snapshot_);
    lv_draw_buf_destroy(menu_snapshot_); menu_snapshot_ = nullptr;
}
void View::prepare_menu_snapshot() {
    // Live serial counters can change forty times a second. Sample passive
    // content at 4 Hz; button/selection/theme changes still refresh immediately.
    if (menu_snapshot_ && menu_version_ == model_.menu_version && old_tab_ == model_.tab &&
        old_selected_ == model_.selected && old_theme_ == model_.theme && now_ >= menu_snapshot_checked_us_ &&
        now_ - menu_snapshot_checked_us_ < 250000) return;
    menu_snapshot_checked_us_ = now_;
    // Snapshot a derived preview. The View never mutates controller state.
    menu_preview_ = model_;
    if (model_.stage == MenuStage::Pick) {
        menu_preview_->tab = model_.remembered_section(); menu_preview_->selected = 0; menu_preview_->stage = MenuStage::Browse;
    }
    const auto& menu = *menu_preview_;
    std::ostringstream signature;
    signature << menu.side << ':' << menu.tab << ':' << menu.selected << ':' << menu.theme << ':'
              << (menu.stage == MenuStage::Edit) << ':' << menu.selected_help(now_);
    for (const auto& row : menu.rows(now_)) signature << '\n' << row.id << ':' << row.value;
    const std::string key = signature.str();
    if (menu_snapshot_ && key == menu_snapshot_signature_) {
        menu_preview_.reset(); return;
    }
    const float progress = menu_progress_;
    const int active_side = active_menu_side_;
    active_menu_side_ = model_.side; menu_progress_ = 1; capturing_menu_ = true;
    lv_obj_set_pos(menu_, active_menu_side_ ? width_ - lv_obj_get_width(menu_) : 0, height_ * 6 / 100);
    drop_menu_snapshot();
    // The blade is entirely opaque while moving. XRGB avoids an unnecessary
    // per-pixel source-alpha blend when copying its cached image to ARGB.
    menu_snapshot_ = lv_snapshot_take(menu_, LV_COLOR_FORMAT_XRGB8888);
    capturing_menu_ = false; menu_progress_ = progress; active_menu_side_ = active_side;
    menu_preview_.reset();
    lv_obj_set_pos(menu_, active_side ? width_ - lv_obj_get_width(menu_) : 0, height_ * 6 / 100);
    if (menu_snapshot_) {
        menu_snapshot_signature_ = key; ++menu_snapshot_count_;
        lv_obj_invalidate(menu_);
    }
}
void View::draw_event(lv_event_t* e) {
    auto* self = static_cast<View*>(lv_event_get_user_data(e));
    auto* target = lv_event_get_target(e);
    auto* layer = lv_event_get_layer(e);
    if (target == self->menu_) self->draw_menu(layer);
    else if (target == self->picker_) self->draw_picker(layer);
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
    auto v = model_.visual;
    if (!v.visible && !model_.menu_open) return;
    if (!v.visible) { v.visible = true; v.rule = v.shade = v.block = 1; v.idle_dim = v.rail_dim = 0.65f; }
    // The unfolded blade replaces this instrument. Do not paint/fade the HUD
    // underneath its translucent backdrop: that adds a second alpha-composite
    // pass over the same large area on every animation frame.
    if ((left ? 0 : 1) == active_menu_side_ && menu_progress_ > 0) return;
    const auto& th = hud_theme_at(model_.theme);
    const auto& t = model_.backend.vrx;
    const bool picked = model_.menu_open && model_.stage == MenuStage::Pick && model_.side == (left ? 0 : 1);
    const auto& bf = model_.backend.flight;
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
        const float reach = picked ? 252 : 252 * v.rule;
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
                line.width = picked ? std::max(2, height_ / 270) : std::max(1, height_ / 360);
                line.color = color(picked || (left && bf.arm == 1) ? th.accent : rest);
                const float taper = std::clamp((span[0] - 126) / 126, 0.0f, 1.0f);
                line.opa = opacity((1 - taper * taper) * (picked ? 1 - menu_progress_ : 0.8f * v.rail_dim));
                lv_draw_line(layer, &line);
            }
        }
    }
    // Original two-track layout and labels. No FC flags, no invented readings.
    const char* battery_label = bf.have_pack_v && !bf.have_cell_v ? "PACK" : "CELL";
    const auto rc = model_.rc_signal(now_);
    const float label_width = left ? text_.design_advance(battery_label, canopy::kTextLabel) :
        std::max(text_.design_advance(rc.label(), canopy::kTextLabel), text_.design_advance("VIDEO", canopy::kTextLabel));
    const float start = 14 + label_width + 12;
    text_.panel(layer, left ? battery_label : rc.label(), left, 14, 527, canopy::kTextLabel, !left, th.text, content);
    text_.panel(layer, left ? "SAG" : "VIDEO", left, 14, 537, canopy::kTextLabel, !left, th.accent, content);
    const float cell_level = bf.have_cell_v ? std::clamp((bf.cell_v - 3.3f) / 0.9f, 0.0f, 1.0f) : 0;
    track(layer, left, start, 271, 525, 9, 20, left ? cell_level : rc.level(), th.data, content);
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
    if (!model_.show_details) return;
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
void View::draw_picker(lv_layer_t* layer) {
    const auto& th = hud_theme_at(model_.theme);
    const float alpha = 1 - menu_progress_;
    for (int side = 0; side < 2; ++side) {
        const bool selected = side == model_.side;
        text_.panel(layer, side ? "AIR UNIT" : "GOGGLE", side == 0, 56, 412,
                    canopy::kTextState, side != 0, selected ? th.text : th.quiet, alpha * (selected ? 1 : 0.6f));
        text_.panel(layer, selected ? "OK TO OPEN" : "LEFT / RIGHT", side == 0, 56, 439,
                    canopy::kTextRow, side != 0, th.quiet, alpha * 0.8f);
    }
}
lv_area_t View::menu_reveal(float progress) const {
    // Ease all the way to an empty strip at the blade's lower edge. Ending at
    // the canopy asset's top left a large rectangle to disappear when hidden.
    // Quintic easing also makes the last 50-Hz pose only a few pixels tall.
    const float p = std::clamp(progress, 0.0f, 1.0f);
    const float e = std::clamp(p * p * p * (p * (p * 6 - 15) + 10), 0.0f, 1.0f);
    const int h = int(std::lround((height_ * 92 / 100) * e));
    const int y = height_ * 6 / 100 + height_ * 92 / 100 - h;
    const int w = int(width_ * 0.31f + (lv_obj_get_width(menu_) - width_ * 0.31f) * e);
    const int x = active_menu_side_ ? width_ - w : 0;
    return {x, y, x + w - 1, y + h - 1};
}
void View::draw_menu(lv_layer_t* layer) {
    const auto& menu_model = menu_preview_ ? *menu_preview_ : model_;
    const auto& th = hud_theme_at(model_.theme);
    lv_area_t bounds;
    lv_obj_get_coords(menu_, &bounds);
    const int x = bounds.x1, y = bounds.y1, w = lv_obj_get_width(menu_), h = height_ * 92 / 100;
    const float u = height_ / 1080.0f;
    const int pad = int(22 * u), row_h = int(50 * u);
    const auto reveal = menu_reveal(menu_progress_);
    if (reveal.x1 > reveal.x2 || reveal.y1 > reveal.y2) return;
    const int backdrop_x = reveal.x1, backdrop_y = reveal.y1;
    const int backdrop_w = reveal.x2 - reveal.x1 + 1, backdrop_h = reveal.y2 - reveal.y1 + 1;
    if (!capturing_menu_ && menu_snapshot_) {
        // Reveal a single completed plane. Its opaque backing lets LVGL copy
        // most pixels, avoiding rerendering/blending all menu primitives.
        const lv_area_t previous_clip = layer->_clip_area;
        lv_area_t clipped{std::max(previous_clip.x1, reveal.x1), std::max(previous_clip.y1, reveal.y1),
                          std::min(previous_clip.x2, reveal.x2), std::min(previous_clip.y2, reveal.y2)};
        if (clipped.x1 <= clipped.x2 && clipped.y1 <= clipped.y2) {
            layer->_clip_area = clipped;
            lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d); d.src = menu_snapshot_; d.opa = LV_OPA_COVER;
            lv_area_t image_area{x, y, x + int(menu_snapshot_->header.w) - 1, y + int(menu_snapshot_->header.h) - 1};
            lv_draw_image(layer, &d, &image_area);
            layer->_clip_area = previous_clip;
        }
        return;
    }
    // Keep the blade opaque when settled too: fading back to 94% requires a
    // costly full-plane blend exactly at the animation's final keyframe.
    rect(layer, backdrop_x, backdrop_y, backdrop_w, backdrop_h, th.ground, 1);
    rect(layer, backdrop_x, backdrop_y, backdrop_w, std::max(1, int(3 * u)), th.accent, menu_progress_);
    if (menu_progress_ < 0.5f) return;
    const float content_alpha = std::clamp((menu_progress_ - 0.5f) * 2, 0.0f, 1.0f);
    text_.canvas(layer, active_menu_side_ ? "AIR UNIT" : "GOGGLE", pad, pad + 14 * u, 15 * u, false, th.accent, content_alpha, x, y);
    int tab_x = pad;
    const std::vector<int> tabs = active_menu_side_ ? std::vector<int>{0, 6, 5} : std::vector<int>{1, 3, 2, 4};
    for (int tab : tabs) {
        const std::string title = Model::tab_name(tab);
        const int tw = int(text_.advance(title) * 21 * u / canopy::kFontPixels);
        text_.canvas(layer, title, tab_x, pad + 52 * u, 21 * u, false,
                      tab == menu_model.tab ? th.text : th.quiet, content_alpha, x, y);
        if (tab == menu_model.tab) rect(layer, x + tab_x, y + pad + int(61 * u), tw, std::max(1, int(3 * u)), th.accent, 1);
        tab_x += tw + int(26 * u);
    }
    const int list_top = pad + int(78 * u);
    rect(layer, x + pad, y + list_top - int(6 * u), w - 2 * pad, 1, th.quiet, 0.3f);
    const auto rows = menu_model.rows(now_);
    const bool editing = menu_model.stage == MenuStage::Edit;
    const int extra = editing ? int(80 * u) : 0;
    const int help_y = h - int(150 * u);
    const int visible = std::max(1, (help_y - list_top - extra - int(12 * u)) / row_h);
    const int start = std::clamp(menu_model.selected - visible / 2, 0, std::max(0, int(rows.size()) - visible));
    int ry = list_top;
    for (int i = start; i < std::min(int(rows.size()), start + visible); ++i) {
        const bool selected = i == menu_model.selected;
        const bool expanded = selected && editing;
        const int rh = row_h + (expanded ? extra : 0);
        if (selected) {
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, th.ground, content_alpha);
            rect(layer, x + pad / 2, y + ry + 2, w - pad, rh - 4, th.accent, content_alpha, std::max(1, int(3 * u)));
        }
        const float baseline = ry + row_h * 0.5f + 23 * u * 0.36f;
        text_.canvas(layer, rows[i].name, pad, baseline, 23 * u, false, th.text, content_alpha * (selected ? 1 : 0.82f), x, y);
        if (expanded) {
            const int current = menu_model.option_index(), n = int(rows[i].options.size());
            for (int delta = -1; delta <= 1; ++delta) {
                const int choice = (current + delta + n) % n;
                const float size = (delta == 0 ? 30 : 18) * u;
                text_.canvas(layer, rows[i].options[choice], w - pad, ry + rh * 0.5f + delta * 34 * u + size * 0.36f,
                             size, true, delta == 0 ? th.accent : th.quiet, content_alpha * (delta == 0 ? 1 : 0.7f), x, y);
            }
            ry += rh; continue;
        }
        const float available = w - 2 * pad - text_.advance(rows[i].name) * 23 * u / canopy::kFontPixels - 16 * u;
        const float value_width = text_.advance(rows[i].value) * 20 * u / canopy::kFontPixels;
        const float size = value_width > available && available > 0 ? std::max(12 * u, 20 * u * available / value_width) : 20 * u;
        text_.canvas(layer, rows[i].value, w - pad, baseline, size, true,
                     selected ? th.accent : th.quiet, content_alpha, x, y);
        ry += rh;
    }
    rect(layer, x + pad, y + help_y, w - 2 * pad, 1, th.quiet, 0.35f);
    std::istringstream words(menu_model.selected_help(now_));
    std::string word, line; int help_line = 0;
    auto emit_line = [&]() {
        text_.canvas(layer, line, pad, help_y + (28 + help_line * 26) * u, 18 * u, false, th.quiet, content_alpha * 0.85f, x, y);
        ++help_line; line.clear();
    };
    while (words >> word && help_line < 3) {
        const auto candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_.advance(candidate) * 18 * u / canopy::kFontPixels > w - 2 * pad) emit_line();
        line = line.empty() ? word : line + " " + word;
    }
    if (!line.empty() && help_line < 3) emit_line();
    text_.canvas(layer, editing ? "OK APPLY   /   BACK CANCEL" : "ARROWS NAVIGATE   /   OK EDIT   /   BACK FOLD", pad,
                 h - 18 * u, 14 * u, false, th.accent, content_alpha * 0.8f, x, y);
}
void View::update(uint64_t now) {
    text_.begin_frame();
    now_ = now;
    const float dt = last_tick_ ? std::min(0.1f, float(now - last_tick_) / 1000000) : 0;
    last_tick_ = now;
    const bool unfolded = model_.menu_open && model_.stage != MenuStage::Pick;
    // Do not rebuild on telemetry changes during motion. A bounded snapshot
    // is refreshed in the picker or when the fully opened content changes.
    // Keep the outgoing blade intact during closure, even though the model has
    // already returned to Pick (or the user changes the next side quickly).
    if (model_.menu_open && (menu_progress_ == 0 || (unfolded && menu_progress_ == 1))) prepare_menu_snapshot();
    const float target = unfolded ? 1 : 0;
    const auto old_reveal = menu_reveal(menu_progress_);
    const float next = menu_progress_ + std::clamp(target - menu_progress_, -dt / 0.24f, dt / 0.24f);
    const bool moving = next != menu_progress_;
    const bool wing_visibility_changed = (menu_progress_ == 0) != (next == 0);
    animating_ = moving || model_.visual.animating;
    menu_progress_ = next;
    if (unfolded) active_menu_side_ = model_.side;
    const bool right_side = active_menu_side_ != 0;
    const int menu_w = lv_obj_get_width(menu_);
    const int mx = right_side ? width_ - menu_w : 0;
    if (moving || old_tab_ != model_.tab || model_.menu_version != menu_version_) lv_obj_set_pos(menu_, mx, height_ * 6 / 100);
    hidden(menu_, next <= 0);
    // Reveal the two-wing picker after the closing blade has settled. Fading
    // it in during closure redraws both HUD wings for every moving frame.
    hidden(picker_, !model_.menu_open || model_.stage != MenuStage::Pick || menu_progress_ > 0);
    const auto& t = model_.backend.vrx;
    const auto& flight = model_.backend.flight;
    const bool flight_fresh = flight.stamp_us && now >= flight.stamp_us && now - flight.stamp_us < 2000000;
    if (flight_fresh && flight.have_cell_v) cell_peak_ = std::max(cell_peak_, flight.cell_v);
    const bool fresh = t.fresh(now), valid = t.link_valid(now);
    const bool hud_visible = model_.hud_visible();
    for (auto* panel : panels_) hidden(panel, !hud_visible);
    const bool appearance = !initialized_ || old_theme_ != model_.theme || old_fresh_ != fresh || old_valid_ != valid || old_details_ != model_.show_details || old_hud_visible_ != hud_visible;
    const bool data = !initialized_ || model_.backend.flight_version != flight_version_ || flight_fresh != old_flight_fresh_ ||
        t.connected != presented_.connected || t.mcs != presented_.mcs ||
        t.frequency_raw != presented_.frequency_raw || t.distance_raw != presented_.distance_raw ||
        t.quality_raw != presented_.quality_raw || t.voltage_mv != presented_.voltage_mv ||
        t.cpu_temperature_x100 != presented_.cpu_temperature_x100 || t.rf_temperature_raw != presented_.rf_temperature_raw;
    const bool menu = old_menu_ != model_.menu_open || old_tab_ != model_.tab || old_selected_ != model_.selected || model_.menu_version != menu_version_;
    const bool counters = t.status_frames != presented_.status_frames || t.unknown_frames != presented_.unknown_frames ||
                          t.bad_frames != presented_.bad_frames || model_.rejected_inputs != rejected_inputs_;
    if (appearance || (!moving && menu) || (moving && next == 0) ||
        (!moving && !menu_snapshot_ && (data || (model_.menu_open && (model_.tab == 2 || model_.tab == 4 || model_.tab == 6) && counters))))
        lv_obj_invalidate(menu_);
    else if (moving) {
        // The cached plane is stationary: pixels shared by the two reveals
        // have not changed. Repaint only newly exposed/removed edge strips,
        // rather than copying the whole growing blade every frame.
        const auto reveal = menu_reveal(next);
        const lv_area_t shared{std::max(old_reveal.x1, reveal.x1), std::max(old_reveal.y1, reveal.y1),
                               std::min(old_reveal.x2, reveal.x2), std::min(old_reveal.y2, reveal.y2)};
        auto invalidate = [&](lv_area_t area) {
            if (area.x1 <= area.x2 && area.y1 <= area.y2) lv_obj_invalidate_area(menu_, &area);
        };
        for (const auto area : {old_reveal, reveal}) {
            if (shared.x1 > shared.x2 || shared.y1 > shared.y2) { invalidate(area); continue; }
            invalidate({area.x1, area.y1, area.x2, shared.y1 - 1});
            invalidate({area.x1, shared.y2 + 1, area.x2, area.y2});
            invalidate({area.x1, shared.y1, shared.x1 - 1, shared.y2});
            invalidate({shared.x2 + 1, shared.y1, area.x2, shared.y2});
        }
    }
    if (appearance || menu) lv_obj_invalidate(picker_);
    if (appearance || data || model_.visual.animating || (menu && next == 0))
        for (auto* p : panels_) lv_obj_invalidate(p);
    else if (wing_visibility_changed) lv_obj_invalidate(panels_[active_menu_side_]);
    hidden(status_, valid || !hud_visible);
    if (!valid) {
        if (appearance || data) lv_obj_invalidate(status_);
    }
    presented_ = t; initialized_ = true;
    old_fresh_ = fresh; old_valid_ = valid; old_theme_ = model_.theme;
    old_menu_ = model_.menu_open; old_tab_ = model_.tab; old_selected_ = model_.selected;
    menu_version_ = model_.menu_version; old_details_ = model_.show_details;
    old_hud_visible_ = hud_visible;
    rejected_inputs_ = model_.rejected_inputs;
    flight_version_ = model_.backend.flight_version; old_flight_fresh_ = flight_fresh;
}
} // namespace kestrel_ui
