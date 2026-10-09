#include "stock_serial.hpp"
#include <cstdio>
#include <cstring>

namespace kestrel_ui {
namespace {
constexpr size_t max_payload = 4096, max_frame = max_payload + 9;
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
int16_t i16(const uint8_t* p) { const auto raw = u16(p); int16_t v; std::memcpy(&v, &raw, 2); return v; }
uint8_t crc8(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) crc = uint8_t((crc << 1) ^ (crc & 0x80 ? 0xd5 : 0));
    return crc;
}
bool fresh(uint64_t stamp, uint64_t now) { return stamp && now >= stamp && now - stamp < 2000000; }
bool printable(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i] < 32 || p[i] > 126) return false;
    return true;
}
}
const char* msp_name(uint16_t function) {
    switch (function) {
    case 1: return "API_VERSION";
    case 2: return "FC_VARIANT";
    case 3: return "FC_VERSION";
    case 101: return "STATUS";
    case 110: return "ANALOG";
    case 150: return "STATUS_EX";
    case 182: return "DISPLAYPORT";
    default: return "OTHER";
    }
}
void StockSerial::reset() {
    buffer_.clear(); flight_ = {}; status_us_ = analog_us_ = 0;
    state_.stamp_us = 0; state_.variant.clear(); state_.firmware.clear(); state_.last_function = 0;
}
bool StockSerial::receive(const StockFrame& frame, uint64_t now) {
    if (frame.channel != 0 || frame.payload.empty()) return false;
    // The 18.21.10 callback at 0x41f628 changes sky IDs 2/4 into prefixes 0/1 and
    // forwards them via FIFO 1 -> fxn_snd_mspdata_to_gui -> channel 0.
    // Prefix 1 is a distinct vendor notification, not an MSP stream.
    if (frame.payload[0] == 1) { ++state_.vendor_notifications; return true; }
    if (frame.payload[0] != 0) return false;
    ++state_.envelopes;
    state_.bytes += frame.payload.size() - 1;
    if (frame.payload.size() == 1) { ++state_.malformed; return true; }
    size_t offset = 1;
    while (offset < frame.payload.size()) {
        const size_t count = std::min(max_frame - buffer_.size(), frame.payload.size() - offset);
        buffer_.insert(buffer_.end(), frame.payload.begin() + offset, frame.payload.begin() + offset + count);
        offset += count;
        drain(now);
        if (buffer_.size() == max_frame) { buffer_.erase(buffer_.begin()); ++state_.malformed; }
    }
    return true;
}
void StockSerial::drain(uint64_t now) {
    size_t offset = 0;
    while (offset < buffer_.size()) {
        const auto* p = buffer_.data() + offset;
        const size_t left = buffer_.size() - offset;
        if (p[0] != '$') { ++offset; ++state_.discarded_bytes; continue; }
        if (left < 2) break;
        if (p[1] != 'M' && p[1] != 'X') { ++offset; ++state_.discarded_bytes; continue; }
        if (left < 3) break;
        if (p[2] != '>' && p[2] != '<' && p[2] != '!') { ++offset; ++state_.malformed; continue; }
        const bool v2 = p[1] == 'X';
        const size_t header = v2 ? 8 : 5;
        if (left < header) break;
        const size_t length = v2 ? u16(p + 6) : p[3];
        if (length > max_payload) { ++offset; ++state_.malformed; continue; }
        const size_t total = header + length + 1;
        if (left < total) break;
        uint8_t check = 0;
        for (size_t i = 3; i < total - 1; ++i) check = v2 ? crc8(check, p[i]) : check ^ p[i];
        if (check != p[total - 1]) { ++offset; ++state_.malformed; continue; }
        message(v2 ? u16(p + 4) : p[4], p + header, length, p[2], now);
        offset += total;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
}
void StockSerial::message(uint16_t function, const uint8_t* p, size_t n, uint8_t direction, uint64_t now) {
    ++state_.frames; state_.stamp_us = now; state_.last_function = function;
    // Requests and error replies establish framing only, never flight values.
    if (direction == '<') { ++state_.requests; return; }
    if (direction == '!') { ++state_.errors; return; }
    switch (function) {
    case 2:
        if (n != 4 || !printable(p, n)) { ++state_.malformed; return; }
        state_.variant.assign(reinterpret_cast<const char*>(p), n);
        return;
    case 3: {
        if (n < 3) { ++state_.malformed; return; }
        char version[32]; std::snprintf(version, sizeof(version), "%u.%u.%u", p[0], p[1], p[2]);
        state_.firmware = version;
        // The captured Betaflight response adds a length-prefixed printable
        // version string (e.g. 2026.12.0-alpha) after the three numeric bytes.
        if (n >= 4 && p[3] && p[3] <= 64 && n == size_t(p[3]) + 4 && printable(p + 4, p[3]))
            state_.firmware.assign(reinterpret_cast<const char*>(p + 4), p[3]);
        return;
    }
    case 101:
    case 150:
        if (n < 11) { ++state_.malformed; return; }
        // Betaflight MSP_STATUS modeFlags bit 0 is BOXARM, as in MspOsd.
        flight_.arm = p[6] & 1;
        status_us_ = now; ++state_.status_frames;
        return;
    case 110: {
        if (n != 7 && n != 9) { ++state_.malformed; return; }
        float pack = p[0] / 10.0f;
        if (n == 9 && u16(p + 7)) pack = u16(p + 7) / 100.0f;
        flight_.have_pack_v = pack > 2.5f; flight_.pack_v = pack;
        flight_.have_amps = true; flight_.amps = i16(p + 5) / 100.0f;
        const uint16_t rssi = u16(p + 3);
        flight_.have_rssi = rssi <= 1023; flight_.rssi_pct = (rssi * 100 + 511) / 1023;
        analog_us_ = now; ++state_.analog_frames;
        return;
    }
    default: ++state_.unhandled; return;
    }
}
void StockSerial::publish(Model& model, uint64_t now) const {
    model.serial = state_;
    BfTelem next;
    next.stamp_us = std::max(status_us_, analog_us_);
    if (fresh(status_us_, now)) next.arm = flight_.arm;
    if (fresh(analog_us_, now)) {
        next.have_pack_v = flight_.have_pack_v; next.pack_v = flight_.pack_v;
        next.have_amps = flight_.have_amps; next.amps = flight_.amps;
        next.have_rssi = flight_.have_rssi; next.rssi_pct = flight_.rssi_pct;
    }
    const auto& old = model.flight;
    if (old.arm != next.arm || old.have_pack_v != next.have_pack_v || old.pack_v != next.pack_v ||
        old.have_amps != next.have_amps || old.amps != next.amps ||
        old.have_rssi != next.have_rssi || old.rssi_pct != next.rssi_pct) ++model.flight_version;
    model.flight = next;
}
} // namespace kestrel_ui
