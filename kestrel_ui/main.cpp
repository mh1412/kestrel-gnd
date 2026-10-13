#include "display.hpp"
#include "stock_backend.hpp"
#include "view.hpp"
#include "input.hpp"
#include "preferences.hpp"
#include "hud_theme.hpp"
#include "ui/frame_cadence.hpp"
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
    std::string hud_executable;
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
        else if (arg == "--hud-executable") o.hud_executable = value();
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
                      "  --hud-executable FILE Passive renderer executable (default: sibling kestrel-hud)\n"
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
std::string hud_executable(const Options& o) {
    if (!o.hud_executable.empty()) return o.hud_executable;
    char path[4096]; const ssize_t n = readlink("/proc/self/exe", path, sizeof(path));
    if (n <= 0 || n == ssize_t(sizeof(path))) throw std::runtime_error("Cannot locate sibling HUD executable");
    const std::string executable(path, size_t(n));
    return executable.substr(0, executable.find_last_of('/') + 1) + "kestrel-hud";
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
            kestrel_ui::Controller controller(model);
            auto preferences = kestrel_ui::load_preferences(o.config);
            if (o.config.empty() || o.fps_override) preferences.fps = o.fps;
            model.set_preferences(preferences);
            std::printf("PREFERENCES theme=%s fps=%d details=%d repeat=%d file=%s\n", hud_theme_at(model.theme).name,
                        model.fps, model.show_details, model.key_repeat, o.config.empty() ? "session-only" : o.config.c_str());
            model.display_width = display.width(); model.display_height = display.height();
            lv_timer_set_period(lv_display_get_refr_timer(display.lv_display()), 1000 / model.fps);
            int applied_fps = model.fps;
            if (!o.config.empty()) controller.save_preferences = [&](const kestrel_ui::UiPreferences& p) { kestrel_ui::store_preferences(o.config, p); };
            if (o.show_menu) controller.open_menu();
            kestrel_ui::HudClient hud(display.width(), display.height(), o.assets, hud_executable(o));
            kestrel_ui::View view(controller, display.width(), display.height(), o.assets, &hud);
            kestrel_ui::Input input(o.input, o.control, o.stock_keys);
            model.input_sources = input.sources(); model.control_path = o.control;
            const uint64_t started = kestrel_ui::monotonic_us();
            uint64_t next_tick = started, next_log = started, render_us = 0, ticks = 0;
            uint64_t animated_ticks = 0, animated_paints = 0;
            std::vector<uint64_t> animation_costs;
            uint64_t demo_tick = 0;
            std::printf("KESTREL_UI backend=%s display=%s cadence=%d size=%dx%d\n", o.demo ? "DEMO" : "stock",
                        o.framebuffer.empty() ? "offscreen" : o.framebuffer.c_str(), model.fps, display.width(), display.height());
            while (!stop && !model.quit) {
                const uint64_t now = kestrel_ui::monotonic_us();
                if (o.seconds && now - started >= uint64_t(o.seconds) * 1000000) break;
                if (backend) backend->poll(model.backend, now);
                else if (now / 400000 != demo_tick) {
                    demo_tick = now / 400000;
                    auto& t = model.backend.vrx;
                    t.stamp_us = now; ++t.status_frames;
                    t.connected = ((now - started) / 1000000) % 12 < 9;
                    t.mcs = t.connected ? 4 : -1;
                    t.voltage_mv = 11512; t.cpu_temperature_x100 = 4889; t.rf_temperature_raw = 45;
                    t.frequency_raw = 5800;
                    if (t.connected) {
                        // Explicit host/offscreen simulation, never populated
                        // by the stock adapter without validated FC forwarding.
                        auto& f = model.backend.flight;
                        f.stamp_us = now;
                        f.have_cell_v = f.have_pack_v = f.have_amps = f.have_speed = f.have_lq = f.have_mode = f.have_timer = true;
                        f.cell_v = 4.10f - 0.08f * std::sin(float(now - started) / 2000000);
                        f.pack_v = f.cell_v * 4; f.amps = 5.6f; f.speed_kmh = 72; f.lq_pct = 94;
                        f.arm = 1; f.timer_s = 194 + int((now - started) / 1000000);
                        std::snprintf(f.mode, sizeof(f.mode), "ACRO");
                        ++model.backend.flight_version;
                    }
                }
                input.poll(controller, now);
                if (menu_requested) { menu_requested = 0; controller.key(kestrel_ui::Key::Menu); }
                if (model.fps != applied_fps) {
                    applied_fps = model.fps;
                    lv_timer_set_period(lv_display_get_refr_timer(display.lv_display()), 1000 / applied_fps);
                    next_tick = now;
                }
                if (now >= next_tick) {
                    const uint64_t frame_started = kestrel_ui::monotonic_us(), paints_before = display.presentations;
                    controller.tick(now); view.update(now); lv_timer_handler();
                    // The application owns animation cadence. LVGL's timer
                    // may not yet be due after variable update/draw work; force
                    // this tick's damage out rather than skipping its pose.
                    lv_refr_now(display.lv_display());
                    model.hud_pid = hud.pid(); model.hud_restarts = hud.restarts();
                    if (view.animating()) {
                        ++animated_ticks;
                        if (display.presentations != paints_before) ++animated_paints;
                        if (animation_costs.size() == 256) animation_costs.erase(animation_costs.begin());
                        animation_costs.push_back(kestrel_ui::monotonic_us() - frame_started);
                    }
                    render_us += kestrel_ui::monotonic_us() - now; ++ticks;
                    const uint64_t period = 1000000 / model.fps;
                    const uint64_t finished = kestrel_ui::monotonic_us();
                    // Keep absolute cadence without accumulating poll jitter,
                    // and skip missed deadlines instead of chasing stale poses.
                    next_tick = kestrel_ui::next_frame_deadline(next_tick, finished, period);
                }
                if (now >= next_log) {
                    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
                    const auto& t = model.backend.vrx;
                    std::printf("STATUS frames=%llu unknown=%llu bad=%llu layout_bytes=%u connected=%d mcs=%d voltage_mv=%d cpu_x100=%d state=\"%s\" rss_kib=%ld render_avg_us=%llu flushes=%llu bytes=%llu text_bytes=%zu text_rasters=%llu paints=%llu snapshots=%llu\n",
                        (unsigned long long)t.status_frames, (unsigned long long)t.unknown_frames, (unsigned long long)t.bad_frames,
                        t.status_payload_bytes, t.connected, t.mcs, t.voltage_mv, t.cpu_temperature_x100, model.status(now).c_str(), usage.ru_maxrss,
                        (unsigned long long)(ticks ? render_us / ticks : 0), (unsigned long long)display.flushes,
                        (unsigned long long)display.bytes_written, view.text_cache_bytes(),
                        (unsigned long long)view.text_rasterizations(), (unsigned long long)display.presentations,
                        (unsigned long long)view.menu_snapshots());
                    if (!animation_costs.empty()) {
                        auto sorted = animation_costs; std::sort(sorted.begin(), sorted.end());
                        uint64_t sum = 0; for (auto cost : sorted) sum += cost;
                        std::printf("ANIMATION ticks=%llu painted=%llu recent_avg_us=%llu recent_p95_us=%llu recent_max_us=%llu\n",
                                    (unsigned long long)animated_ticks, (unsigned long long)animated_paints,
                                    (unsigned long long)(sum / sorted.size()),
                                    (unsigned long long)sorted[(sorted.size() - 1) * 95 / 100], (unsigned long long)sorted.back());
                    }
                    const auto& serial = model.backend.serial;
                    std::printf("HUD pid=%d frames=%llu restarts=%llu render_us=%llu shared_bytes=%zu\n", hud.pid(),
                        (unsigned long long)hud.frames(), (unsigned long long)hud.restarts(),
                        (unsigned long long)hud.render_us(), hud.layout().bytes() * 2);
                    if (serial.envelopes || serial.vendor_notifications) {
                        const auto& flight = model.backend.flight;
                        const auto rc = model.rc_signal(now);
                        std::printf("SERIAL envelopes=%llu bytes=%llu msp=%llu status=%llu analog=%llu malformed=%llu discarded=%llu other=%llu requests=%llu errors=%llu vendor=%llu variant=\"%s\" firmware=\"%s\" active=%d arm=%d pack_v=%.2f amps=%.2f rc_source=\"%s\" rc_pct=%d\n",
                            (unsigned long long)serial.envelopes, (unsigned long long)serial.bytes,
                            (unsigned long long)serial.frames, (unsigned long long)serial.status_frames,
                            (unsigned long long)serial.analog_frames, (unsigned long long)serial.malformed,
                            (unsigned long long)serial.discarded_bytes, (unsigned long long)serial.unhandled,
                            (unsigned long long)serial.requests, (unsigned long long)serial.errors,
                            (unsigned long long)serial.vendor_notifications, serial.variant.c_str(), serial.firmware.c_str(),
                            serial.fresh(now), flight.arm, flight.have_pack_v ? flight.pack_v : -1.0f,
                            flight.have_amps ? flight.amps : -1.0f, rc.label(), rc.percent);
                    }
                    next_log = now + 2000000;
                }
                usleep(2000);
            }
            display.verify_framebuffer();
            if (!o.screenshot.empty()) display.dump(o.screenshot);
            std::printf("EXIT frames=%llu ticks=%llu flushes=%llu screenshot=%s\n", (unsigned long long)model.backend.vrx.status_frames,
                        (unsigned long long)ticks, (unsigned long long)display.flushes, o.screenshot.c_str());
        }
        lv_deinit();
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "KestrelUI: %s\n", e.what()); return 1; }
}
