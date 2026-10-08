#pragma once
#include "ui/canopy.hpp"
#include "msp_osd.hpp"
#include <string>
#include <vector>

namespace kestrel_ui {
uint64_t monotonic_us();
struct Telemetry {
    uint64_t stamp_us = 0;
    bool connected = false;
    int mcs = -1, quality_raw = 0, frequency_raw = 0, distance_raw = 0;
    int voltage_mv = 0, cpu_temperature_x100 = 0, rf_temperature_raw = 0;
    unsigned status_payload_bytes = 0;
    uint64_t status_frames = 0, unknown_frames = 0, bad_frames = 0;
    bool fresh(uint64_t now) const { return stamp_us && now >= stamp_us && now - stamp_us < 2000000; }
    bool link_valid(uint64_t now) const { return fresh(now) && connected && mcs >= 0; }
};

enum class Key { Menu, Up, Down, Left, Right, Enter, Back, Quit };
struct MenuRow { std::string name, value; bool editable = false; };

// A graphics-independent initial OSD facade. Backend settings remain backend
// owned. Only local visual preferences are editable until stock setters and
// their readback/persistence semantics have been validated.
class Model {
public:
    Telemetry telemetry;
    BfTelem flight; // Same have_* semantics as the original OSD; stock adapter does not invent FC data.
    uint64_t flight_version = 0;
    bool menu_open = false, quit = false;
    int tab = 0, selected = 0, theme = 0;
    canopy::LinkVisual visual;
    void tick(uint64_t now);
    void key(Key key);
    std::string status(uint64_t now) const;
    std::vector<MenuRow> rows(uint64_t now) const;
    static const char* tab_name(int tab);
private:
    bool previous_ = false;
    uint64_t up_ = 0, down_ = 0;
};
} // namespace kestrel_ui
