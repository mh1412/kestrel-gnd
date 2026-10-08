#include "stock_protocol.hpp"
#include <cstring>
#include <stdexcept>

namespace kestrel_ui {
namespace {
constexpr size_t max_frame = 0x5000;
int32_t le32(const uint8_t* p) {
    const uint32_t raw = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    int32_t value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}
}
std::vector<uint8_t> stock_packet(const StockFrame& f) {
    if (f.payload.size() > max_frame - 8) throw std::invalid_argument("Stock payload too large");
    std::vector<uint8_t> out = {0x8e, 0x25, 0, f.sequence, f.channel, f.flags,
                              uint8_t(f.payload.size()), uint8_t(f.payload.size() >> 8)};
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    for (size_t i = 3; i < out.size(); ++i) out[2] ^= out[i];
    return out;
}
bool stock_status(const StockFrame& f, Telemetry& t, uint64_t now) {
    // 17.5.3 sends subtype + 200 bytes. The live 18.21.10 ELF's
    // Fxn_period_msg_to_gui (0x41b6bc) sends subtype + 296 bytes; disassembly
    // confirms all fields below retain their offsets. Do not accept arbitrary
    // longer structures or interpret the newer extension as old telemetry.
    if (f.channel != 1 || (f.payload.size() != 201 && f.payload.size() != 297) || f.payload[0] != 1) return false;
    const auto* p = f.payload.data() + 1;
    t.quality_raw = le32(p);
    t.mcs = le32(p + 4);
    t.connected = le32(p + 8) != 0;
    t.frequency_raw = le32(p + 12);
    t.distance_raw = le32(p + 16);
    t.voltage_mv = le32(p + 40);
    t.cpu_temperature_x100 = le32(p + 100);
    t.rf_temperature_raw = static_cast<int8_t>(p[173]);
    t.status_payload_bytes = f.payload.size();
    t.stamp_us = now;
    ++t.status_frames;
    return true;
}
void StockParser::feed(const uint8_t* data, size_t size, const std::function<void(const StockFrame&)>& receive) {
    while (size) {
        const size_t count = std::min(size, max_frame - buffer_.size());
        buffer_.insert(buffer_.end(), data, data + count);
        data += count; size -= count;
        drain(receive);
        if (buffer_.size() == max_frame) { buffer_.erase(buffer_.begin()); ++rejected; }
    }
}
void StockParser::drain(const std::function<void(const StockFrame&)>& receive) {
    size_t offset = 0;
    while (offset + 1 < buffer_.size()) {
        const auto* p = buffer_.data() + offset;
        if (p[0] != 0x8e || p[1] != 0x25) { ++offset; continue; }
        if (buffer_.size() - offset < 8) break;
        const size_t length = size_t(p[6]) | size_t(p[7]) << 8;
        if (length > max_frame - 8) { ++offset; ++rejected; continue; }
        if (buffer_.size() - offset < length + 8) break;
        uint8_t checksum = 0;
        for (size_t i = 3; i < length + 8; ++i) checksum ^= p[i];
        if (checksum != p[2]) { ++offset; ++rejected; continue; }
        StockFrame f{p[3], p[4], p[5], {p + 8, p + 8 + length}};
        receive(f);
        offset += length + 8;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
}
} // namespace kestrel_ui
