#include "display.hpp"
#include "stock_backend.hpp"
#include "view.hpp"
#include "input.hpp"
#include "preferences.hpp"
#include "hud_theme.hpp"
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <linux/input.h>
#include <memory>
#include <stdexcept>
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 2 || LVGL_VERSION_PATCH != 2
#error "KestrelUI currently requires the pinned LVGL 9.2.2 API/configuration"
#endif

namespace {
volatile sig_atomic_t stop = 0, menu_requested = 0;
void signal_handler(int sig) { if (sig == SIGUSR1) menu_requested = 1; else stop = 1; }
uint32_t tick() { return uint32_t(kestrel_ui::monotonic_us() / 1000); }
struct Options {
    int width = 1920, height = 1080, port = 2765, fps = 25, seconds = 0;
    bool demo = false, show_menu = false, stock_keys = false, interactive = false, fps_override = false;
    std::string assets, framebuffer, screenshot, input, control, config, send_key;
};
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error("Missing value for " + arg); return argv[i]; };
        if (arg == "--demo") o.demo = true;
        else if (arg == "--stock") o.demo = false;
        else if (arg == "--show-menu") o.show_menu = true;
        else if (arg == "--stock-keys") o.stock_keys = true;
        else if (arg == "--interactive-control") o.interactive = true;
        else if (arg == "--assets") o.assets = value();
        else if (arg == "--fb") o.framebuffer = value();
        else if (arg == "--screenshot") o.screenshot = value();
        else if (arg == "--input") o.input = value();
        else if (arg == "--control") o.control = value();
        else if (arg == "--config") o.config = value();
        else if (arg == "--send-key") o.send_key = value();
        else if (arg == "--port") o.port = std::stoi(value());
        else if (arg == "--width") o.width = std::stoi(value());
        else if (arg == "--height") o.height = std::stoi(value());
        else if (arg == "--fps") { o.fps = std::stoi(value()); o.fps_override = true; }
        else if (arg == "--seconds") o.seconds = std::stoi(value());
        else if (arg == "--help") {
            std::puts("kestrel-ui --assets DIR [--stock | --demo] [--fb /dev/fb0]\n"
                      "  --width N --height N   Offscreen resolution (default 1920x1080)\n"
                      "  --port N              Loopback stock GUI server (default 2765)\n"
                      "  --fps N               Maximum UI cadence, 1..60 (default 25)\n"
                      "  --seconds N           Exit after a bounded run; 0 runs until stopped\n"
                      "  --screenshot FILE     Save the final transparent overlay as RGBA PAM\n"
                      "  --show-menu           Start with the canopy side picker open\n"
                      "  --input /dev/input/eventN  Optional evdev keyboard\n"
                      "  --stock-keys          Receive validated menu keys on localhost UDP 6222\n"
                      "  --control SOCKET      Local UI-control datagram socket (owner-only)\n"
                      "  --config FILE         Save local UI preferences on OK; no backend settings\n"
                      "  --control SOCKET --send-key KEY  Send menu/up/down/left/right/enter/back/status\n"
                      "  --control SOCKET --interactive-control  SSH keyboard controller; Q exits client\n"
                      "  stdin: Enter/M opens; arrows/WASD choose/navigate/edit; Esc backs/cancels; Q quits UI\n"
                      "  SIGUSR1 toggles menu; SIGINT/SIGTERM restore framebuffer and exit.\n"
                      "  This prototype sends no RF, camera, recording or configuration commands.");
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (o.interactive || !o.send_key.empty()) {
        if (o.control.empty() || (o.interactive && !o.send_key.empty())) throw std::runtime_error("Choose a client mode and --control SOCKET");
        return o;
    }
    if (o.assets.empty()) throw std::runtime_error("--assets DIR is required (generate with kestrel-ui-assetgen)");
    if (o.width < 640 || o.width > 4096 || o.height < 360 || o.height > 2160 ||
        o.fps < 1 || o.fps > 60 || o.port < 1 || o.port > 65535 || o.seconds < 0 || o.seconds > 86400)
        throw std::runtime_error("Invalid resolution, cadence, port or duration");
    return o;
}
}
int main(int argc, char** argv) {
    try {
        const auto o = options(argc, argv);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        struct sigaction action{};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        for (int s : {SIGINT, SIGTERM, SIGUSR1}) sigaction(s, &action, nullptr);
        if (!o.send_key.empty()) { std::puts(kestrel_ui::send_control(o.control, o.send_key).c_str()); return 0; }
        if (o.interactive) return kestrel_ui::interactive_control(o.control, stop);
        // Bind before writing to the framebuffer: a running GlassesUI causes
        // a clean failure rather than two competing UI processes.
        std::unique_ptr<kestrel_ui::StockBackend> backend;
        if (!o.demo) backend = std::make_unique<kestrel_ui::StockBackend>(o.port);
        lv_init();
        lv_tick_set_cb(tick);
        {
            kestrel_ui::Display display(o.width, o.height, o.framebuffer);
            kestrel_ui::Model model;
            auto preferences = kestrel_ui::load_preferences(o.config);
            if (o.config.empty() || o.fps_override) preferences.fps = o.fps;
            model.set_preferences(preferences);
            std::printf("PREFERENCES theme=%s fps=%d details=%d repeat=%d file=%s\n", hud_theme_at(model.theme).name,
                        model.fps, model.show_details, model.key_repeat, o.config.empty() ? "session-only" : o.config.c_str());
            model.display_width = display.width(); model.display_height = display.height();
            lv_timer_set_period(lv_display_get_refr_timer(display.lv_display()), 1000 / model.fps);
            int applied_fps = model.fps;
            if (!o.config.empty()) model.save_preferences = [&](const kestrel_ui::UiPreferences& p) { kestrel_ui::store_preferences(o.config, p); };
            if (o.show_menu) model.open_menu();
            kestrel_ui::View view(model, display.width(), display.height(), o.assets);
            kestrel_ui::Input input(o.input, o.control, o.stock_keys);
            model.input_sources = input.sources(); model.control_path = o.control;
            const uint64_t started = kestrel_ui::monotonic_us();
            uint64_t next_tick = started, next_log = started, render_us = 0, ticks = 0;
            uint64_t demo_tick = 0;
            std::printf("KESTREL_UI backend=%s display=%s cadence=%d size=%dx%d\n", o.demo ? "DEMO" : "stock",
                        o.framebuffer.empty() ? "offscreen" : o.framebuffer.c_str(), model.fps, display.width(), display.height());
            while (!stop && !model.quit) {
                const uint64_t now = kestrel_ui::monotonic_us();
                if (o.seconds && now - started >= uint64_t(o.seconds) * 1000000) break;
                if (backend) backend->poll(model.telemetry, now);
                else if (now / 400000 != demo_tick) {
                    demo_tick = now / 400000;
                    auto& t = model.telemetry;
                    t.stamp_us = now; ++t.status_frames;
                    t.connected = ((now - started) / 1000000) % 12 < 9;
                    t.mcs = t.connected ? 4 : -1;
                    t.voltage_mv = 11512; t.cpu_temperature_x100 = 4889; t.rf_temperature_raw = 45;
                    t.frequency_raw = 5800;
                    if (t.connected) {
                        // Explicit host/offscreen simulation, never populated
                        // by the stock adapter without validated FC forwarding.
                        auto& f = model.flight;
                        f.stamp_us = now;
                        f.have_cell_v = f.have_pack_v = f.have_amps = f.have_speed = f.have_lq = f.have_mode = f.have_timer = true;
                        f.cell_v = 4.10f - 0.08f * std::sin(float(now - started) / 2000000);
                        f.pack_v = f.cell_v * 4; f.amps = 5.6f; f.speed_kmh = 72; f.lq_pct = 94;
                        f.arm = 1; f.timer_s = 194 + int((now - started) / 1000000);
                        std::snprintf(f.mode, sizeof(f.mode), "ACRO");
                        ++model.flight_version;
                    }
                }
                input.poll(model, now);
                if (menu_requested) { menu_requested = 0; model.key(kestrel_ui::Key::Menu); }
                if (model.fps != applied_fps) {
                    applied_fps = model.fps;
                    lv_timer_set_period(lv_display_get_refr_timer(display.lv_display()), 1000 / applied_fps);
                    next_tick = now;
                }
                if (now >= next_tick) {
                    model.tick(now); view.update(now); lv_timer_handler();
                    render_us += kestrel_ui::monotonic_us() - now; ++ticks;
                    next_tick = now + 1000000 / model.fps;
                }
                if (now >= next_log) {
                    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
                    const auto& t = model.telemetry;
                    std::printf("STATUS frames=%llu unknown=%llu bad=%llu layout_bytes=%u connected=%d mcs=%d voltage_mv=%d cpu_x100=%d state=\"%s\" rss_kib=%ld render_avg_us=%llu flushes=%llu bytes=%llu text_bytes=%zu text_rasters=%llu\n",
                        (unsigned long long)t.status_frames, (unsigned long long)t.unknown_frames, (unsigned long long)t.bad_frames,
                        t.status_payload_bytes, t.connected, t.mcs, t.voltage_mv, t.cpu_temperature_x100, model.status(now).c_str(), usage.ru_maxrss,
                        (unsigned long long)(ticks ? render_us / ticks : 0), (unsigned long long)display.flushes,
                        (unsigned long long)display.bytes_written, view.text_cache_bytes(),
                        (unsigned long long)view.text_rasterizations());
                    next_log = now + 2000000;
                }
                usleep(2000);
            }
            display.verify_framebuffer();
            if (!o.screenshot.empty()) display.dump(o.screenshot);
            std::printf("EXIT frames=%llu ticks=%llu flushes=%llu screenshot=%s\n", (unsigned long long)model.telemetry.status_frames,
                        (unsigned long long)ticks, (unsigned long long)display.flushes, o.screenshot.c_str());
        }
        lv_deinit();
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "KestrelUI: %s\n", e.what()); return 1; }
}
