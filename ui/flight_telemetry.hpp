#pragma once
#include <cstdint>

// Shared, graphics-independent flight values. Unavailable fields stay false;
// moving this type out of MspOsd lets backend/UI/HUD contracts use it without
// importing the legacy OSD parser, physics, mutexes or renderer resources.
struct BfTelem {
    bool have_cell_v = false; float cell_v = 0.0f;
    bool have_pack_v = false; float pack_v = 0.0f;
    bool have_amps = false; float amps = 0.0f;
    bool have_alt = false; float alt_m = 0.0f;
    bool have_speed = false; float speed_kmh = 0.0f;
    bool have_rssi = false; int rssi_pct = 0;
    bool have_dbm = false; int rssi_dbm = 0;
    bool have_lq = false; int lq_pct = 0;
    bool have_timer = false; int timer_s = 0;
    bool have_mode = false; char mode[8] = {0};
    bool have_pitch = false; float pitch_deg = 0.0f;
    bool have_roll = false; float roll_deg = 0.0f;
    bool have_batt_pct = false; int batt_pct = 0;
    bool have_gps = false; int gps_fix = 0, sats = 0;
    bool have_pos = false; double lat = 0.0, lon = 0.0;
    bool have_home = false; int home_dist_m = 0, home_dir_deg = 0;
    int arm = -1;
    uint64_t stamp_us = 0;
};
