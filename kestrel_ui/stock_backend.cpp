#include "stock_backend.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace kestrel_ui {
StockBackend::StockBackend(int port) {
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create stock telemetry socket");
    int yes = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
    // The old GUI's accepted connection can briefly hold the address during
    // replacement. Retry only at startup, before touching the framebuffer.
    int bound = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        bound = bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (!bound || errno != EADDRINUSE) break;
        usleep(100000);
    }
    if (bound || listen(listener_, 1)) {
        const int error = errno;
        close(listener_); listener_ = -1;
        throw std::runtime_error("Cannot listen on stock GUI port " + std::to_string(port) + ": " + std::to_string(error));
    }
    std::printf("STOCK listening=127.0.0.1:%d\n", port);
}
StockBackend::~StockBackend() { disconnect(); if (listener_ >= 0) close(listener_); }
void StockBackend::disconnect() {
    if (client_ >= 0) close(client_);
    client_ = -1; parser_.reset(); serial_.reset(); replies_.clear();
}
void StockBackend::poll(Model& model, uint64_t now) {
    auto& state = model.telemetry;
    // Accept at most one extra connection each tick; never let a connection
    // flood starve rendering. The already connected backend remains owner.
    int incoming = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (incoming >= 0) {
        if (client_ >= 0) close(incoming);
        else { client_ = incoming; std::puts("STOCK backend connected"); }
    }
    if (client_ < 0) { serial_.publish(model, now); return; }
    uint8_t data[4096];
    for (int reads = 0; reads < 8; ++reads) {
        const ssize_t n = recv(client_, data, sizeof(data), 0);
        if (n == 0) { disconnect(); break; }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) disconnect();
            break;
        }
        parser_.feed(data, size_t(n), [&](const StockFrame& f) {
            if (!stock_status(f, state, now) && !serial_.receive(f, now)) {
                ++state.unknown_frames;
                if (state.unknown_frames <= 8)
                    std::printf("STOCK unmapped channel=%u flags=0x%02x length=%zu subtype=%u\n",
                                f.channel, f.flags, f.payload.size(), f.payload.empty() ? 0 : f.payload[0]);
            }
            // Protocol-level ACK only; no MSP polling or application writes.
            if (f.flags & 0x80) {
                auto reply = stock_packet({f.sequence, f.channel, uint8_t(f.flags & 0x7f), {}});
                if (replies_.size() + reply.size() <= 4096) replies_.insert(replies_.end(), reply.begin(), reply.end());
            }
        });
    }
    state.bad_frames = parser_.rejected;
    serial_.publish(model, now);
    if (client_ < 0 || replies_.empty()) return;
    std::vector<uint8_t> out(replies_.begin(), replies_.end());
    const ssize_t sent = send(client_, out.data(), out.size(), MSG_NOSIGNAL);
    if (sent > 0) replies_.erase(replies_.begin(), replies_.begin() + sent);
    else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) disconnect();
}
} // namespace kestrel_ui
