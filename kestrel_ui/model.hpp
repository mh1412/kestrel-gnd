#pragma once
#include "ui/canopy.hpp"
#include "msp_osd.hpp"
#include <string>
#include <vector>
#include <functional>

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
enum class MenuStage { Pick, Browse, Edit };
struct UiPreferences {
    int theme = 0, fps = 25;
    bool details = true, repeat = true;
};
struct MenuRow {
    std::string name, value;
    bool editable = false;
    std::string id, help;
    std::vector<std::string> options;
};

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
    int side = 0, fps = 25;
    bool show_details = true, key_repeat = true;
    MenuStage stage = MenuStage::Pick;
    uint64_t menu_version = 0, key_count = 0, rejected_inputs = 0;
    std::string last_key = "--", input_sources = "stdin", control_path;
    std::string feedback;
    int display_width = 1920, display_height = 1080;
    std::function<void(const UiPreferences&)> save_preferences;
    canopy::LinkVisual visual;
    void tick(uint64_t now);
    void key(Key key, bool repeat = false);
    void open_menu();
    UiPreferences preferences() const { return {theme, fps, show_details, key_repeat}; }
    void set_preferences(const UiPreferences& p);
    std::vector<int> side_tabs() const;
    int remembered_section() const;
    std::string describe() const;
    std::string selected_help(uint64_t now) const;
    int option_index() const;
    std::string status(uint64_t now) const;
    std::vector<MenuRow> rows(uint64_t now) const;
    static const char* tab_name(int tab);
private:
    bool previous_ = false;
    uint64_t up_ = 0, down_ = 0;
    int remembered_tab_[2] = {1, 0}, edit_original_ = 0;
    int edit_tab_ = -1;
    std::string edit_id_;
    int setting(const std::string& id) const;
    void set_setting(const std::string& id, int value);
    void cancel_edit();
    bool edit_intact() const;
};
const char* key_name(Key key);
bool parse_key(const std::string& text, Key& key);
} // namespace kestrel_ui
