#include "stock_serial.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<uint8_t> hex(const char* text) {
    std::istringstream in(text); unsigned byte; std::vector<uint8_t> bytes;
    while (in >> std::hex >> byte) bytes.push_back(uint8_t(byte));
    return bytes;
}
kestrel_ui::StockFrame envelope(const std::vector<uint8_t>& bytes) { return {0, 0, 0, bytes}; }
std::vector<uint8_t> v1(uint8_t function, std::vector<uint8_t> payload, char direction = '>') {
    std::vector<uint8_t> wire{0, '$', 'M', uint8_t(direction), uint8_t(payload.size()), function};
    wire.insert(wire.end(), payload.begin(), payload.end());
    uint8_t checksum = 0; for (size_t i = 4; i < wire.size(); ++i) checksum ^= wire[i];
    wire.push_back(checksum); return wire;
}
}
int main(int argc, char** argv) {
    using namespace kestrel_ui;
    try {
        if (argc == 3 && std::string(argv[1]) == "--stream") {
            std::ifstream in(argv[2], std::ios::binary);
            if (!in) throw std::runtime_error("Cannot open stock GUI stream");
            StockParser parser; StockSerial serial; Model model;
            std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
            parser.feed(bytes.data(), bytes.size(), [&](const StockFrame& frame) {
                if (!stock_status(frame, model.telemetry, 1000000) && !serial.receive(frame, 1000000)) ++model.telemetry.unknown_frames;
            });
            serial.publish(model, 1000000);
            std::printf("REPLAY status=%llu unknown=%llu outer_bad=%llu serial=%llu msp=%llu status_msp=%llu analog=%llu malformed=%llu vendor=%llu variant=%s firmware=%s arm=%d pack_v=%.2f amps=%.2f remaining=%zu\n",
                (unsigned long long)model.telemetry.status_frames, (unsigned long long)model.telemetry.unknown_frames,
                (unsigned long long)parser.rejected, (unsigned long long)model.serial.envelopes,
                (unsigned long long)model.serial.frames, (unsigned long long)model.serial.status_frames,
                (unsigned long long)model.serial.analog_frames, (unsigned long long)model.serial.malformed,
                (unsigned long long)model.serial.vendor_notifications, model.serial.variant.c_str(), model.serial.firmware.c_str(),
                model.flight.arm, model.flight.pack_v, model.flight.amps, serial.buffered());
            check(!parser.rejected && !parser.buffered() && !serial.buffered() && !model.serial.malformed, "Replay rejected or truncated data");
            return 0;
        }
        // Actual 18.21.10 capture: a stock prefix, intact checksum and FC bytes.
        const auto status = hex("00 24 4d 3e 1b 65 7c 00 00 00 23 00 00 00 00 00 00 19 00 00 00 00 1e 04 01 00 00 00 20 00 04 03 00 04");
        const auto analog = hex("00 24 4d 3e 09 6e 74 1b 00 00 00 00 00 86 04 8a");
        const auto variant = hex("00 24 4d 3e 04 02 42 54 46 4c 1a");
        const auto version = hex("00 24 4d 3e 13 03 1a 0c 00 0f 32 30 32 36 2e 31 32 2e 30 2d 61 6c 70 68 61 65");
        StockSerial serial; Model model;
        check(!serial.receive({0, 1, 0, status}, 1000000), "Do not parse the wrong outer channel as MSP");
        check(!serial.receive({0, 0, 0, {2, '$', 'M'}}, 1000000), "Do not guess unknown stock prefixes");
        for (size_t i = 1; i < status.size(); ++i) {
            check(serial.receive({0, 0, 0, {0, status[i]}}, 1000000), "Fragmented stock forwarding envelope");
            serial.publish(model, 1000000);
            check(model.serial.frames == (i == status.size() - 1 ? 1u : 0u), "Do not commit partial MSP data");
        }
        serial.receive(envelope(analog), 1000000); serial.receive(envelope(variant), 1000000); serial.receive(envelope(version), 1000000);
        serial.publish(model, 1000000);
        check(model.serial.frames == 4 && !model.serial.malformed, "Capture fixture checksums");
        check(model.serial.variant == "BTFL" && model.serial.firmware == "2026.12.0-alpha", "Actual FC identity/version");
        check(model.flight.arm == 0 && model.flight.have_pack_v && std::abs(model.flight.pack_v - 11.58f) < 0.001f &&
              model.flight.have_amps && model.flight.amps == 0 && !model.flight.have_cell_v, "Actual pack/current/disarmed data without guessed cell count");
        const auto unchanged = model.flight_version;
        serial.receive(envelope(status), 1100000); serial.publish(model, 1100000);
        check(model.flight_version == unchanged, "Repeated unchanged STATUS does not redraw the HUD");
        auto joined = analog; joined.insert(joined.end(), status.begin() + 1, status.end());
        serial.receive(envelope(joined), 1200000); serial.publish(model, 1200000);
        check(model.serial.frames == 7, "Multiple MSP frames in one stock envelope");
        auto corrupt = analog; corrupt.back() ^= 1;
        corrupt.insert(corrupt.end(), status.begin() + 1, status.end());
        serial.receive(envelope(corrupt), 1300000); serial.publish(model, 1300000);
        check(model.serial.frames == 8 && model.serial.malformed == 1, "Corrupt MSP resynchronizes to a valid frame");
        serial.receive(envelope(status), 2700000); serial.receive(envelope(variant), 3300000);
        serial.publish(model, 3300000);
        check(model.flight.arm == 0 && !model.flight.have_pack_v && !model.flight.have_amps,
              "Frequent STATUS/identity must not keep old analog readings alive");
        serial.publish(model, 4700000);
        check(model.flight.arm == -1, "Arm state expires independently");
        const auto before_vendor = model.serial.frames;
        serial.receive({0, 0, 0, {1, '$', 'M', '>', 0, 0, 0}}, 4800000); serial.publish(model, 4800000);
        check(model.serial.vendor_notifications == 1 && model.serial.frames == before_vendor,
              "Vendor prefix 1 is never an MSP stream");
        serial.reset(); serial.publish(model, 4800000);
        check(!model.flight.stamp_us && model.flight.arm == -1 && model.serial.variant.empty(), "Disconnect drops peer state but retains counters");
        std::vector<uint8_t> armed(11); armed[6] = 1;
        serial.receive(envelope(v1(101, armed, '<')), 4900000);
        serial.receive(envelope(v1(101, armed, '!')), 4900000); serial.publish(model, 4900000);
        check(model.serial.requests == 1 && model.serial.errors == 1 && model.flight.arm == -1,
              "Requests/error replies cannot supply flight state");
        serial.receive(envelope(v1(101, armed)), 5000000); serial.publish(model, 5000000);
        check(model.flight.arm == 1, "Validated status response supplies armed bit");
        serial.receive(envelope(v1(110, {1, 2})), 5100000);
        serial.receive(envelope(v1(182, {0})), 5100000); serial.publish(model, 5100000);
        check(model.serial.malformed == 2 && model.serial.unhandled == 1, "Short known payload and valid unhandled MSP are distinct");
        // Oversized v2 length and arbitrarily long garbage must stay bounded.
        serial.receive(envelope(hex("00 24 58 3e 00 6e 00 01 10")), 5200000);
        serial.receive(envelope(std::vector<uint8_t>(100000, 0)), 5200000);
        check(serial.buffered() < 4105, "Bounded serial parser");
        serial.receive(envelope(analog), 5300000); serial.publish(model, 5300000);
        check(model.flight.have_pack_v, "Parser recovers after oversized headers/garbage");
        const auto v2 = hex("00 24 58 3e 00 6e 00 09 00 2a 00 00 ff 03 9c ff a4 01 2b");
        serial.receive(envelope(v2), 5400000); serial.publish(model, 5400000);
        check(std::abs(model.flight.pack_v - 4.2f) < 0.001f && model.flight.amps == -1 && model.flight.rssi_pct == 100,
              "Native MSP v2 DVB-S2 checksum and signed current");
        auto bad_v2 = v2; bad_v2.back() ^= 1;
        serial.receive(envelope(bad_v2), 5500000); serial.publish(model, 5500000);
        check(model.flight.pack_v == 4.2f, "Invalid v2 checksum cannot overwrite telemetry");
        serial.receive(envelope({0, '$', 'M', '>', 9, 110, 42}), 5600000);
        check(serial.buffered() != 0, "Partial frame retained before disconnect");
        serial.reset(); serial.receive(envelope(variant), 5700000); serial.publish(model, 5700000);
        check(!serial.buffered() && model.serial.variant == "BTFL" && !model.flight.have_pack_v,
              "Old partial frame cannot cross a backend reconnect");
        // Captured during the RC-bar investigation: ANALOG RSSI = 0x01d1.
        const auto rc_analog = hex("00 24 4d 3e 09 6e 77 00 00 d1 01 00 00 a5 04 61");
        serial.receive(envelope(rc_analog), 5800000); serial.publish(model, 5800000);
        check(model.flight.rssi_pct == 45 && !model.flight.have_lq &&
              model.rc_signal(5800000).source == RcSource::Rssi && model.rc_signal(5800000).percent == 45,
              "Actual stock RSSI supplies the RC bar without being mislabeled LQ");
        model.flight.have_lq = true; model.flight.lq_pct = 94;
        check(model.rc_signal(5800000).source == RcSource::LinkQuality && model.rc_signal(5800000).percent == 94,
              "Actual link quality takes priority over RSSI");
        model.flight.lq_pct = 0;
        check(model.rc_signal(5800000).source == RcSource::LinkQuality && model.rc_signal(5800000).percent == 0,
              "Zero LQ must not fall back to a stronger RSSI");
        model.flight.lq_pct = 101;
        check(model.rc_signal(5800000).source == RcSource::Rssi, "Invalid LQ does not mask valid RSSI");
        model.flight.have_lq = false; model.flight.rssi_pct = 0;
        check(model.rc_signal(5800000).source == RcSource::Rssi && model.rc_signal(5800000).percent == 0,
              "Valid zero RSSI is distinct from unavailable data");
        model.flight.rssi_pct = 101;
        check(model.rc_signal(5800000).source == RcSource::Unavailable, "Out-of-range RSSI is not drawn");
        serial.publish(model, 5800000);
        check(model.rc_signal(5799999).source == RcSource::Unavailable &&
              model.rc_signal(7800000).source == RcSource::Unavailable, "Backwards/stale RC readings are not drawn");
        serial.receive(envelope(status), 7900000); serial.publish(model, 7900000);
        check(model.rc_signal(7900000).source == RcSource::Unavailable,
              "Fresh STATUS must not keep an expired RSSI bar filled");
        std::puts("PASS: captured MSP fixtures, stock fragmentation/coalescing, checksums, peer reset, independent field expiry and bounded recovery");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
