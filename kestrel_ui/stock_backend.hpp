#pragma once
#include "stock_serial.hpp"
#include <deque>

namespace kestrel_ui {
// The stock backend is a client of the UI's 2765 server. This is not a second
// radio client and does not initialize or modify vendor media/RF state.
class StockBackend {
public:
    explicit StockBackend(int port);
    ~StockBackend();
    StockBackend(const StockBackend&) = delete;
    StockBackend& operator=(const StockBackend&) = delete;
    void poll(Model& model, uint64_t now);
private:
    int listener_ = -1, client_ = -1;
    StockParser parser_;
    StockSerial serial_;
    std::deque<uint8_t> replies_;
    void disconnect();
};
} // namespace kestrel_ui
