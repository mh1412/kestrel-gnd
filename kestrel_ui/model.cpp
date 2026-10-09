#include "model.hpp"
#include "stock_serial.hpp"
#include "hud_theme.hpp"
#include <cctype>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace kestrel_ui {
namespace {
MenuRow reading(std::string id, std::string name, std::string value, std::string help) {
    return {std::move(name), std::move(value), false, std::move(id), std::move(help), {}};
}
MenuRow choice(std::string id, std::string name, int current, std::vector<std::string> options, std::string help) {
    const std::string value = options.at(size_t(current));
    return {std::move(name), value, true, std::move(id), std::move(help), std::move(options)};
}
}
uint64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
const char* key_name(Key key) {
    static const char* names[] = {"menu", "up", "down", "left", "right", "enter", "back", "quit"};
    return names[static_cast<unsigned>(key)];
}
bool parse_key(const std::string& text, Key& key) {
    if (text.size() > 32) return false;
    std::string normalized;
    for (unsigned char c : text) normalized += char(std::tolower(c));
    while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.back()))) normalized.pop_back();
    size_t first = normalized.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    normalized.erase(0, first);
    if (normalized == "ok" || normalized == "return") normalized = "enter";
    if (normalized == "esc" || normalized == "cancel") normalized = "back";
    for (unsigned i = 0; i < 8; ++i) if (normalized == key_name(static_cast<Key>(i))) { key = static_cast<Key>(i); return true; }
    return false;
}
void Model::tick(uint64_t now) {
    visual = canopy::link_visual(telemetry.link_valid(now), now, previous_, up_, down_);
    if (menu_open) {
        const auto menu = rows(now);
        if (stage == MenuStage::Edit && !edit_intact()) cancel_edit();
        selected = std::clamp(selected, 0, std::max(0, int(menu.size()) - 1));
    }
}
std::string Model::status(uint64_t now) const {
    if (!telemetry.fresh(now)) return telemetry.stamp_us ? "BACKEND STALE" : "WAITING FOR BACKEND";
    if (!telemetry.connected) return "NO RF LINK";
    if (telemetry.mcs < 0) return "LINK UP / NO VALID MCS";
    return "RF CONNECTED / VIDEO UNVERIFIED";
}
RcSignal Model::rc_signal(uint64_t now) const {
    if (!flight.stamp_us || now < flight.stamp_us || now - flight.stamp_us >= 2000000) return {};
    // RSSI is signal strength, not packet delivery quality. Keep the source
    // distinct and prefer real LQ when a backend supplies it, including 0% LQ.
    if (flight.have_lq && flight.lq_pct >= 0 && flight.lq_pct <= 100)
        return {RcSource::LinkQuality, flight.lq_pct};
    if (flight.have_rssi && flight.rssi_pct >= 0 && flight.rssi_pct <= 100)
        return {RcSource::Rssi, flight.rssi_pct};
    return {};
}
const char* Model::tab_name(int t) {
    static const char* names[] = {"RADIO", "HUD", "SYSTEM", "DISPLAY", "INPUT", "INFO", "SERIAL"};
    return names[t >= 0 && t < 7 ? t : 0];
}
std::vector<int> Model::side_tabs() const { return side == 0 ? std::vector<int>{1, 3, 2, 4} : std::vector<int>{0, 6, 5}; }
int Model::remembered_section() const {
    const auto tabs = side_tabs();
    return std::find(tabs.begin(), tabs.end(), remembered_tab_[side]) == tabs.end() ? tabs.front() : remembered_tab_[side];
}
std::vector<MenuRow> Model::rows(uint64_t now) const {
    const bool fresh = telemetry.fresh(now), valid = telemetry.link_valid(now);
    auto value = [&](int v, bool available) { return available ? std::to_string(v) : "--"; };
    if (tab == 0) return {
        reading("link", "State", status(now), "RF connection is not proof of decoded video. Stock backend owns reception."),
        reading("mcs", "MCS", value(telemetry.mcs, valid), "Raw receive modulation/coding value; unavailable if telemetry is stale or the RF link is down."),
        reading("frequency", "Frequency (raw)", value(telemetry.frequency_raw, valid), "Uncalibrated stock frequency-table value. No frequency command is sent by this menu."),
        reading("distance", "Distance (raw)", value(telemetry.distance_raw, valid), "Raw stock ranging value; units/calibration are not yet validated."),
        reading("quality", "Quality grade (raw)", value(telemetry.quality_raw, valid), "Raw vendor quality enum, not a percentage or signal strength."),
        reading("rf_controls", "RF / camera controls", "Backend-owned", "Configuration writes require validated backend commands and readback. These rows are read-only.")};
    if (tab == 1) {
        std::vector<std::string> themes;
        for (int i = 0; i < hud_theme_count(); ++i) themes.push_back(hud_theme_at(i).name);
        return {choice("theme", "Theme", theme, std::move(themes), "Preview the canopy colors. OK applies; BACK restores the original theme."),
            choice("details", "Detail rows", show_details ? 1 : 0, {"OFF", "ON"}, "Show the aircraft/receiver detail rows beneath the canopy gauges."),
            reading("renderer", "Renderer", "CPU perspective", "Chakra Petch text projected onto the original canopy panel geometry, cached as alpha masks."),
            reading("flight", "Aircraft telemetry", !flight.stamp_us ? "Unavailable" :
                (now >= flight.stamp_us && now - flight.stamp_us < 2000000 ? "Received" : "Stale"),
                "Validated MSP STATUS supplies arm state; ANALOG supplies pack voltage/current. Each expires independently. No cell count or flight mode is guessed.")};
    }
    if (tab == 3) {
        std::vector<std::string> options;
        for (int r = 1; r <= 60; ++r) options.push_back(std::to_string(r) + " Hz");
        return {choice("fps", "UI cadence", fps - 1, std::move(options), "Maximum UI animation/update cadence. This does not change camera FPS or HDMI video mode."),
            reading("canvas", "Overlay canvas", std::to_string(display_width) + "x" + std::to_string(display_height), "Framebuffer geometry read from the current stock display. No modeset is performed."),
            reading("format", "Overlay format", "ARGB8888", "Software alpha overlay, separate from the hardware video plane."),
            reading("video_owner", "Video output", "Stock backend", "The decoder and HDMI video remain owned by ar_ldy_gnd.")};
    }
    if (tab == 4) return {
        choice("repeat", "Arrow repeat", key_repeat ? 1 : 0, {"OFF", "ON"}, "Allow held arrow keys to repeat. OK, MENU and BACK are never repeated."),
        reading("sources", "Sources", input_sources, "All input sources feed the same menu controller. Stock GPIO 4-6 are board-ID straps, not buttons."),
        reading("key_count", "Keys received", std::to_string(key_count), "Accepted normalized menu keys, including the local control socket and SIGUSR1."),
        reading("last_key", "Last key", last_key, "Latest accepted key; BACK cancels an edit, folds a blade, then closes the menu."),
        reading("rejected", "Ignored packets", std::to_string(rejected_inputs), "Malformed/unmapped input packets are ignored; they do not issue radio or recording commands."),
        reading("control", "Control socket", control_path.empty() ? "Disabled" : control_path.substr(control_path.find_last_of('/') + 1), "Use the SSH interactive controller or --send-key to operate a detached UI.")};
    if (tab == 5) return {reading("backend", "Receiver backend", "ar_ldy_gnd", "Stock backend supplies RF/media and normalized GUI status."),
        reading("layout", "Status payload", value(int(telemetry.status_payload_bytes), fresh) + " bytes", "Known 17.5.3/18.21.10 layouts are decoded; unknown extensions are not guessed."),
        reading("capabilities", "Control writes", "Not enabled", "Only local UI settings are editable at this stage.")};
    if (tab == 6) return {
        reading("serial_feed", "MSP feed", serial.fresh(now) ? "Active" : serial.frames ? "Stale" : "Waiting", "Checksum-validated MSP v1/native v2 from the stock forwarding feed. Receiving packets does not imply arming or a valid video stream."),
        reading("fc_variant", "Flight controller", serial.variant.empty() ? "--" : serial.variant, "MSP_FC_VARIANT supplied by the flight controller, for example BTFL = Betaflight."),
        reading("fc_firmware", "FC firmware", serial.firmware.empty() ? "--" : serial.firmware, "MSP_FC_VERSION, including the captured length-prefixed Betaflight version string when present."),
        reading("msp_frames", "MSP frames", std::to_string(serial.frames), "Validated complete MSP frames, including requests/errors counted separately. One stock envelope can contain multiple frames or a partial frame."),
        reading("serial_bytes", "Serial bytes", std::to_string(serial.bytes), "Forwarded serial bytes excluding the stock envelope's one-byte prefix. The UI never opens the physical UART."),
        reading("msp_last", "Last MSP", std::string(msp_name(serial.last_function)) + " (" + std::to_string(serial.last_function) + ")", "Last checksum-validated MSP function. Other valid functions remain counted without guessing their payload semantics."),
        reading("msp_status", "Status messages", std::to_string(serial.status_frames), "MSP_STATUS/STATUS_EX flight mode flags, including the Betaflight BOXARM bit."),
        reading("msp_analog", "Analog messages", std::to_string(serial.analog_frames), "MSP_ANALOG pack voltage, current and RSSI. Pack voltage is not divided by an assumed cell count."),
        reading("rc_signal", "RC indicator", rc_signal(now).source == RcSource::Unavailable ? "Unavailable" :
            std::string(rc_signal(now).label()) + " " + std::to_string(rc_signal(now).percent) + "%",
            "The RC bar prefers actual LQ, otherwise fresh MSP_ANALOG RSSI. RSSI is not relabeled as LQ; stale or unavailable data leaves the bar unfilled."),
        reading("msp_bad", "Malformed MSP", std::to_string(serial.malformed), "Bad direction, length, checksum or decoded payload. Invalid packets never supply flight readings."),
        reading("msp_other", "Other MSP", std::to_string(serial.unhandled), "Valid MSP functions whose data is not decoded yet, including DisplayPort grid messages."),
        reading("vendor_serial", "Vendor notifications", std::to_string(serial.vendor_notifications), "Separate channel-0 prefix-1 stock sky notifications. Contents remain opaque and are not parsed as MSP."),
        reading("notifications", "Unmapped messages", std::to_string(telemetry.unknown_frames), "Other stock envelope types whose framing or routing is not mapped. Valid serial/MSP envelopes are counted above instead."),
        reading("serial_owner", "VRX UART", "Stock backend", "KestrelUI does not open or reconfigure the backend-owned ttyS4 serial port."),
        reading("fc", "FC polling", "Receive-only", "No MSP requests, UART-mode changes or backend control writes are sent.")};
    return {reading("voltage", "Receiver voltage", fresh && telemetry.voltage_mv > 0 ? std::to_string(telemetry.voltage_mv) + " mV" : "--", "Fitted stock supply-voltage measurement."),
        reading("cpu", "CPU temperature", fresh ? std::to_string(telemetry.cpu_temperature_x100 / 100.0f) + " C" : "--", "Receiver CPU temperature, not air-unit temperature."),
        reading("rf_temperature", "RF temperature (raw)", value(telemetry.rf_temperature_raw, fresh), "Raw stock RF-board temperature field."),
        reading("status_count", "Status frames", std::to_string(telemetry.status_frames), "Validated periodic receiver status packets."),
        reading("unknown_count", "Unknown frames", std::to_string(telemetry.unknown_frames), "Correctly framed notifications with unmapped semantics."),
        reading("bad_count", "Rejected frames", std::to_string(telemetry.bad_frames), "Invalid framing, lengths or checksums rejected by the parser.")};
}
int Model::setting(const std::string& id) const {
    if (id == "theme") return theme;
    if (id == "details") return show_details ? 1 : 0;
    if (id == "repeat") return key_repeat ? 1 : 0;
    if (id == "fps") return fps - 1;
    return 0;
}
void Model::set_setting(const std::string& id, int value) {
    if (id == "theme") theme = value;
    else if (id == "details") show_details = value != 0;
    else if (id == "repeat") key_repeat = value != 0;
    else if (id == "fps") fps = std::clamp(value + 1, 1, 60);
}
void Model::set_preferences(const UiPreferences& p) {
    if (p.theme < 0 || p.theme >= hud_theme_count() || p.fps < 1 || p.fps > 60)
        throw std::invalid_argument("Invalid UI preferences");
    theme = p.theme; fps = p.fps; show_details = p.details; key_repeat = p.repeat;
}
void Model::open_menu() { menu_open = true; stage = MenuStage::Pick; feedback.clear(); ++menu_version; }
bool Model::edit_intact() const {
    const auto menu = rows(monotonic_us());
    return tab == edit_tab_ && selected >= 0 && selected < int(menu.size()) && menu[selected].editable && menu[selected].id == edit_id_;
}
void Model::cancel_edit() {
    if (!edit_id_.empty()) set_setting(edit_id_, edit_original_);
    edit_id_.clear(); stage = MenuStage::Browse; feedback = "Change cancelled"; ++menu_version;
}
int Model::option_index() const { return stage == MenuStage::Edit ? setting(edit_id_) : 0; }
void Model::key(Key key, bool repeat) {
    const bool arrow = key == Key::Up || key == Key::Down || key == Key::Left || key == Key::Right;
    if (repeat && (!key_repeat || !arrow)) return;
    ++key_count; last_key = key_name(key); ++menu_version;
    if (key == Key::Quit) { if (stage == MenuStage::Edit) cancel_edit(); quit = true; return; }
    if (key == Key::Menu) {
        if (menu_open) { if (stage == MenuStage::Edit) cancel_edit(); menu_open = false; stage = MenuStage::Pick; }
        else open_menu();
        return;
    }
    if (!menu_open) { if (key == Key::Enter) open_menu(); return; }
    if (stage == MenuStage::Edit) {
        if (!edit_intact()) { cancel_edit(); return; }
        if (key == Key::Back || key == Key::Left) { cancel_edit(); return; }
        const auto menu = rows(monotonic_us());
        if (key == Key::Up || key == Key::Down) {
            const int n = int(menu[selected].options.size());
            if (n) set_setting(edit_id_, (setting(edit_id_) + (key == Key::Down ? 1 : n - 1)) % n);
        } else if (key == Key::Enter) {
            try {
                if (save_preferences) { save_preferences(preferences()); feedback = "UI preference saved"; }
                else feedback = "Applied for this session";
            } catch (const std::exception& error) {
                set_setting(edit_id_, edit_original_);
                feedback = std::string("Save failed; cancelled: ") + std::string(error.what()).substr(0, 80);
            }
            edit_id_.clear(); stage = MenuStage::Browse;
        }
        return;
    }
    if (key == Key::Back) {
        if (stage == MenuStage::Browse) stage = MenuStage::Pick;
        else menu_open = false;
        feedback.clear(); return;
    }
    if (stage == MenuStage::Pick) {
        if (key == Key::Left) side = 0;
        else if (key == Key::Right) side = 1;
        else if (key == Key::Up || key == Key::Down) side = 1 - side; // Three-button controllers can choose too.
        else if (key == Key::Enter) {
            const auto tabs = side_tabs();
            const int remembered = remembered_tab_[side];
            tab = std::find(tabs.begin(), tabs.end(), remembered) == tabs.end() ? tabs.front() : remembered;
            selected = 0; stage = MenuStage::Browse;
        }
        return;
    }
    feedback.clear();
    const auto menu = rows(monotonic_us());
    selected = std::clamp(selected, 0, int(menu.size()) - 1);
    if (key == Key::Up) selected = (selected + int(menu.size()) - 1) % int(menu.size());
    else if (key == Key::Down) selected = (selected + 1) % int(menu.size());
    else if (key == Key::Left || key == Key::Right) {
        const auto tabs = side_tabs();
        auto found = std::find(tabs.begin(), tabs.end(), tab);
        int index = found == tabs.end() ? 0 : int(found - tabs.begin());
        index = std::clamp(index + (key == Key::Right ? 1 : -1), 0, int(tabs.size()) - 1);
        remembered_tab_[side] = tab = tabs[index]; selected = 0;
    } else if (key == Key::Enter && menu[selected].editable) {
        edit_tab_ = tab; edit_id_ = menu[selected].id; edit_original_ = setting(edit_id_); stage = MenuStage::Edit;
    }
}
std::string Model::selected_help(uint64_t now) const {
    if (stage == MenuStage::Edit) return "Preview: UP/DOWN changes. OK applies and saves. BACK cancels.";
    const auto menu = rows(now);
    if (selected < 0 || selected >= int(menu.size())) return "";
    return feedback.empty() ? menu[selected].help : feedback;
}
std::string Model::describe() const {
    std::ostringstream out;
    out << "OK open=" << menu_open << " stage=" << (!menu_open ? "closed" : stage == MenuStage::Pick ? "pick" : stage == MenuStage::Browse ? "browse" : "edit")
        << " side=" << (side ? "air" : "goggle") << " tab=" << tab_name(tab) << " row=" << selected
        << " theme=" << hud_theme_at(theme).name << " fps=" << fps << " details=" << show_details << " repeat=" << key_repeat
        << " keys=" << key_count << " last=" << last_key;
    return out.str();
}
} // namespace kestrel_ui
