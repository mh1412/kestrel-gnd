#pragma once
#include "ui/flight_telemetry.hpp"
#include <string>

namespace kestrel_ui {
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
struct SerialTelemetry {
    uint64_t stamp_us = 0, envelopes = 0, bytes = 0, frames = 0;
    uint64_t malformed = 0, discarded_bytes = 0, requests = 0, errors = 0, unhandled = 0;
    uint64_t status_frames = 0, analog_frames = 0, vendor_notifications = 0;
    uint16_t last_function = 0;
    std::string variant, firmware;
    bool fresh(uint64_t now) const { return stamp_us && now >= stamp_us && now - stamp_us < 2000000; }
};
struct VtxState {
    // FC firmware is not VTX firmware. Empty values mean not yet reported.
    uint64_t stamp_us = 0;
    std::string identity, firmware;
};
struct BackendState {
    Telemetry vrx;
    VtxState vtx;
    SerialTelemetry serial;
    BfTelem flight;
    uint64_t flight_version = 0;
};
} // namespace kestrel_ui
