#include "model.hpp"
#include "hud_theme.hpp"
#include <ctime>

namespace kestrel_ui {
uint64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
void Model::tick(uint64_t now) {
    visual = canopy::link_visual(telemetry.link_valid(now), now, previous_, up_, down_);
}
std::string Model::status(uint64_t now) const {
    if (!telemetry.fresh(now)) return telemetry.stamp_us ? "BACKEND STALE" : "WAITING FOR BACKEND";
    if (!telemetry.connected) return "NO RF LINK";
    if (telemetry.mcs < 0) return "LINK UP / NO VALID MCS";
    // The periodic stock feed does not prove decoded-frame progress.
    return "RF CONNECTED / VIDEO UNVERIFIED";
}
const char* Model::tab_name(int t) {
    static const char* names[] = {"RF LINK", "HUD", "SYSTEM"};
    return names[t >= 0 && t < 3 ? t : 0];
}
std::vector<MenuRow> Model::rows(uint64_t now) const {
    const bool fresh = telemetry.fresh(now), valid = telemetry.link_valid(now);
    auto value = [&](int v, bool available) { return available ? std::to_string(v) : "--"; };
    if (tab == 0) return {
        {"State", status(now)}, {"MCS", value(telemetry.mcs, valid)},
        {"Frequency (raw)", value(telemetry.frequency_raw, valid)},
        {"Distance (raw)", value(telemetry.distance_raw, valid)},
        {"Quality grade (raw)", value(telemetry.quality_raw, valid)},
        {"RF / camera changes", "Not yet validated"}};
    if (tab == 1) return {{"Theme", hud_theme_at(theme).name, true},
        {"Renderer", "LVGL software / canopy"}, {"Aircraft telemetry", "Awaiting validated MSP forwarding"},
        {"Animations", "Shared OSD link sweep / idle pulse"}};
    return {{"Receiver voltage", fresh && telemetry.voltage_mv > 0 ? std::to_string(telemetry.voltage_mv) + " mV" : "--"},
        {"CPU temperature", fresh ? std::to_string(telemetry.cpu_temperature_x100 / 100.0f) + " C" : "--"},
        {"RF temperature (raw)", value(telemetry.rf_temperature_raw, fresh)},
        {"Status frames", std::to_string(telemetry.status_frames)},
        {"Unknown frames", std::to_string(telemetry.unknown_frames)},
        {"Rejected frames", std::to_string(telemetry.bad_frames)}};
}
void Model::key(Key key) {
    if (key == Key::Quit) { quit = true; return; }
    if (key == Key::Menu) { menu_open = !menu_open; return; }
    if (key == Key::Back) { menu_open = false; return; }
    if (!menu_open) return;
    const auto menu = rows(monotonic_us());
    if (key == Key::Up) selected = (selected + int(menu.size()) - 1) % int(menu.size());
    if (key == Key::Down) selected = (selected + 1) % int(menu.size());
    if (key == Key::Enter && menu[selected].editable) theme = (theme + 1) % hud_theme_count();
    if (key == Key::Left || key == Key::Right) {
        tab = (tab + (key == Key::Left ? 2 : 1)) % 3;
        selected = 0;
    }
}
} // namespace kestrel_ui
