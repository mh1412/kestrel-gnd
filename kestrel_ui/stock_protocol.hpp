#pragma once
#include "model.hpp"
#include <functional>
#include <vector>

namespace kestrel_ui {
struct StockFrame {
    uint8_t sequence = 0, channel = 0, flags = 0;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> stock_packet(const StockFrame& frame);
bool stock_status(const StockFrame& frame, Telemetry& state, uint64_t now);

// Handles fragmented/coalesced TCP frames, resynchronizes after corrupt data,
// and never retains more than the vendor's 0x5000-byte receive bound.
class StockParser {
public:
    uint64_t rejected = 0;
    void feed(const uint8_t* data, size_t size, const std::function<void(const StockFrame&)>& receive);
    void reset() { buffer_.clear(); }
    size_t buffered() const { return buffer_.size(); }
private:
    std::vector<uint8_t> buffer_;
    void drain(const std::function<void(const StockFrame&)>& receive);
};
} // namespace kestrel_ui
