#include "display.hpp"
#include "view.hpp"
#include "hud_theme.hpp"
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: kestrel-ui-render-tests GENERATED_1080P_ASSETS");
        lv_init();
        {
            kestrel_ui::Display display(1920, 1080, "");
            kestrel_ui::Model model;
            kestrel_ui::View view(model, 1920, 1080, argv[1]);
            uint64_t now = 1000000;
            for (int frame = 0; frame < 240; ++frame) {
                now += 40000;
                auto& t = model.telemetry;
                t.stamp_us = now; t.connected = true; t.mcs = 1 + frame % 12;
                t.cpu_temperature_x100 = (30 + frame) * 100;
                t.voltage_mv = 11000 + frame * 100; t.frequency_raw = 5800;
                t.status_frames = frame + 1;
                auto& flight = model.flight;
                flight.stamp_us = now;
                flight.have_cell_v = flight.have_amps = flight.have_speed = flight.have_lq = flight.have_timer = flight.have_mode = true;
                flight.cell_v = 4.2f - (frame % 80) * 0.005f;
                flight.amps = 5 + frame % 10; flight.speed_kmh = 50 + frame % 20; flight.lq_pct = 94;
                flight.arm = (frame / 30) % 2; flight.timer_s = frame;
                std::snprintf(flight.mode, sizeof(flight.mode), "ACRO");
                model.flight_version = frame + 1;
                model.menu_open = frame % 40 < 20;
                model.tab = (frame / 20) % 3;
                model.side = model.tab == 0 ? 1 : 0;
                model.stage = frame % 40 < 6 ? kestrel_ui::MenuStage::Pick : kestrel_ui::MenuStage::Browse;
                model.selected = frame % int(model.rows(now).size());
                model.theme = (frame / 30) % hud_theme_count();
                model.tick(now); view.update(now);
                lv_tick_inc(40);
                lv_refr_now(display.lv_display());
                if (view.text_cache_bytes() > 1500 * 1024) throw std::runtime_error("Projected text cache exceeded its frame budget");
            }
            model.menu_open = true; model.side = 0; model.tab = 1; model.selected = 0;
            model.stage = kestrel_ui::MenuStage::Browse;
            model.key(kestrel_ui::Key::Enter);
            for (int frame = 0; frame < 24; ++frame) {
                now += 40000; model.telemetry.stamp_us = now; model.flight.stamp_us = now;
                if (frame % 4 == 0) model.key(kestrel_ui::Key::Down);
                model.tick(now); view.update(now); lv_tick_inc(40); lv_refr_now(display.lv_display());
            }
            model.key(kestrel_ui::Key::Back);
            model.tick(now); view.update(now); lv_refr_now(display.lv_display());
            if (view.text_rasterizations() < 240) throw std::runtime_error("Test did not exercise changing projected text / eviction");
            const auto warm = view.text_rasterizations();
            for (int i = 0; i < 4; ++i) {
                model.tick(now); view.update(now);
                lv_obj_invalidate(lv_screen_active());
                lv_refr_now(display.lv_display());
            }
            if (view.text_rasterizations() != warm) throw std::runtime_error("Unchanged text was rasterized again");
            std::printf("PASS: 240 frames, menu motion/tabs/themes, text LRU lifetime, stable-cache reuse; bytes=%zu rasters=%llu\n",
                        view.text_cache_bytes(), (unsigned long long)view.text_rasterizations());
        }
        lv_deinit();
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
