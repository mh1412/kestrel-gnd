#include "display.hpp"
#include "stock_backend.hpp"
#include "view.hpp"
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
    bool demo = false, show_menu = false;
    std::string assets, framebuffer, screenshot, input;
};
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error("Missing value for " + arg); return argv[i]; };
        if (arg == "--demo") o.demo = true;
        else if (arg == "--stock") o.demo = false;
        else if (arg == "--show-menu") o.show_menu = true;
        else if (arg == "--assets") o.assets = value();
        else if (arg == "--fb") o.framebuffer = value();
        else if (arg == "--screenshot") o.screenshot = value();
        else if (arg == "--input") o.input = value();
        else if (arg == "--port") o.port = std::stoi(value());
        else if (arg == "--width") o.width = std::stoi(value());
        else if (arg == "--height") o.height = std::stoi(value());
        else if (arg == "--fps") o.fps = std::stoi(value());
        else if (arg == "--seconds") o.seconds = std::stoi(value());
        else if (arg == "--help") {
            std::puts("kestrel-ui --assets DIR [--stock | --demo] [--fb /dev/fb0]\n"
                      "  --width N --height N   Offscreen resolution (default 1920x1080)\n"
                      "  --port N              Loopback stock GUI server (default 2765)\n"
                      "  --fps N               Maximum UI cadence, 1..60 (default 25)\n"
                      "  --seconds N           Exit after a bounded run; 0 runs until stopped\n"
                      "  --screenshot FILE     Save the final transparent overlay as RGBA PAM\n"
                      "  --show-menu           Start with the diagnostic menu open\n"
                      "  --input /dev/input/eventN  Optional evdev keyboard\n"
                      "  stdin: m menu, w/s select, a/d tabs, Enter theme, Esc close, q quit\n"
                      "  SIGUSR1 toggles menu; SIGINT/SIGTERM restore framebuffer and exit.\n"
                      "  This prototype sends no RF, camera, recording or configuration commands.");
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (o.assets.empty()) throw std::runtime_error("--assets DIR is required (generate with kestrel-ui-assetgen)");
    if (o.width < 640 || o.width > 4096 || o.height < 360 || o.height > 2160 ||
        o.fps < 1 || o.fps > 60 || o.port < 1 || o.port > 65535 || o.seconds < 0 || o.seconds > 86400)
        throw std::runtime_error("Invalid resolution, cadence, port or duration");
    return o;
}
class Input {
public:
    explicit Input(const std::string& path) {
        original_flags_ = fcntl(STDIN_FILENO, F_GETFL);
        if (original_flags_ >= 0) fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK);
        if (isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &original_term_)) {
            termios raw = original_term_;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
            term_saved_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
        if (!path.empty()) {
            fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd_ < 0) std::fprintf(stderr, "Cannot open input %s: %d\n", path.c_str(), errno);
        }
    }
    ~Input() {
        if (term_saved_) tcsetattr(STDIN_FILENO, TCSANOW, &original_term_);
        if (original_flags_ >= 0) fcntl(STDIN_FILENO, F_SETFL, original_flags_);
        if (fd_ >= 0) close(fd_);
    }
    void poll(kestrel_ui::Model& model) {
        using kestrel_ui::Key;
        char data[64];
        const ssize_t n = read(STDIN_FILENO, data, sizeof(data));
        for (ssize_t i = 0; i < n; ++i) {
            // Preserve escape-sequence state across fragmented terminal reads.
            const char c = data[i];
            if (escape_ == 1 && c == '[') { escape_ = 2; continue; }
            if (escape_ == 2) {
                if (c == 'A') model.key(Key::Up);
                if (c == 'B') model.key(Key::Down);
                if (c == 'C') model.key(Key::Right);
                if (c == 'D') model.key(Key::Left);
                escape_ = 0; continue;
            }
            if (escape_ == 1) { model.key(Key::Back); escape_ = 0; }
            if (c == 27) { escape_ = 1; escape_at_ = kestrel_ui::monotonic_us(); }
            if (c == 'm' || c == 'M') model.key(Key::Menu);
            if (c == 'w') model.key(Key::Up);
            if (c == 's') model.key(Key::Down);
            if (c == 'a') model.key(Key::Left);
            if (c == 'd') model.key(Key::Right);
            if (c == '\n' || c == '\r') model.key(Key::Enter);
            if (c == 'q') model.key(Key::Quit);
        }
        if (escape_ && kestrel_ui::monotonic_us() - escape_at_ > 100000) { model.key(Key::Back); escape_ = 0; }
        input_event event{};
        for (int reads = 0; fd_ >= 0 && reads < 32 && read(fd_, &event, sizeof(event)) == sizeof(event); ++reads) {
            if (event.type != EV_KEY || event.value == 0) continue;
            switch (event.code) {
                case KEY_UP: model.key(Key::Up); break;
                case KEY_DOWN: model.key(Key::Down); break;
                case KEY_LEFT: model.key(Key::Left); break;
                case KEY_RIGHT: model.key(Key::Right); break;
                case KEY_ENTER: model.key(Key::Enter); break;
                case KEY_ESC: case KEY_BACK: model.key(Key::Back); break;
                case KEY_M: case KEY_MENU: model.key(Key::Menu); break;
                case KEY_Q: model.key(Key::Quit); break;
            }
        }
    }
private:
    int original_flags_ = -1, fd_ = -1, escape_ = 0;
    uint64_t escape_at_ = 0;
    termios original_term_{};
    bool term_saved_ = false;
};
}
int main(int argc, char** argv) {
    try {
        const auto o = options(argc, argv);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        struct sigaction action{};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        for (int s : {SIGINT, SIGTERM, SIGUSR1}) sigaction(s, &action, nullptr);
        // Bind before writing to the framebuffer: a running GlassesUI causes
        // a clean failure rather than two competing UI processes.
        std::unique_ptr<kestrel_ui::StockBackend> backend;
        if (!o.demo) backend = std::make_unique<kestrel_ui::StockBackend>(o.port);
        lv_init();
        lv_tick_set_cb(tick);
        {
            kestrel_ui::Display display(o.width, o.height, o.framebuffer);
            kestrel_ui::Model model;
            model.menu_open = o.show_menu;
            kestrel_ui::View view(model, display.width(), display.height(), o.assets);
            Input input(o.input);
            const uint64_t started = kestrel_ui::monotonic_us();
            uint64_t next_tick = started, next_log = started, render_us = 0, ticks = 0;
            uint64_t demo_tick = 0;
            std::printf("KESTREL_UI backend=%s display=%s cadence=%d size=%dx%d\n", o.demo ? "DEMO" : "stock",
                        o.framebuffer.empty() ? "offscreen" : o.framebuffer.c_str(), o.fps, display.width(), display.height());
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
                input.poll(model);
                if (menu_requested) { menu_requested = 0; model.key(kestrel_ui::Key::Menu); }
                if (now >= next_tick) {
                    model.tick(now); view.update(now); lv_timer_handler();
                    render_us += kestrel_ui::monotonic_us() - now; ++ticks;
                    next_tick = now + 1000000 / o.fps;
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
