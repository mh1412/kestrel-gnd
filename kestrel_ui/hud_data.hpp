#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace kestrel_ui {
// Display-ready input for a passive HUD. No stock packets, timestamps to
// reinterpret, user actions, framebuffer handles, LVGL objects or GL resources.
struct HudPanelData {
    bool visible = false, picked = false, fresh = false, armed = false;
    float shade = 0, rule = 0, block = 0, dim = 1, rail_dim = 1;
    float upper_level = 0, lower_level = 0, state_opacity = 0;
    std::string primary = "--", primary_unit, secondary, secondary_unit;
    std::string state, upper_label, lower_label, detail1, detail2;
    bool detail1_quiet = false;
};
struct HudData {
    bool enabled = true, details = true, show_status = true;
    int theme = 0;
    std::string status;
    std::array<HudPanelData, 2> panels;
};
} // namespace kestrel_ui
