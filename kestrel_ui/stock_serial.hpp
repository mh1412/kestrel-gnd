#pragma once
#include "stock_protocol.hpp"

namespace kestrel_ui {
// Read-only decoder for the validated channel-0 stock forwarding envelope.
// Serial fragments can cross envelope boundaries; never send MSP requests.
class StockSerial {
public:
    bool receive(const StockFrame& frame, uint64_t now);
    void publish(Model& model, uint64_t now) const;
    void reset(); // Drop partial data/peer identity/measurements, retain counters.
    size_t buffered() const { return buffer_.size(); }
private:
    SerialTelemetry state_;
    BfTelem flight_;
    uint64_t status_us_ = 0, analog_us_ = 0;
    std::vector<uint8_t> buffer_;
    void drain(uint64_t now);
    void message(uint16_t function, const uint8_t* payload, size_t size, uint8_t direction, uint64_t now);
};
const char* msp_name(uint16_t function);
} // namespace kestrel_ui
