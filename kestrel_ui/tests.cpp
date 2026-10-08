#include "model.hpp"
#include "stock_protocol.hpp"
#include "hud_theme.hpp"
#include "font_atlas.hpp"
#include "utils/math_utils.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <sstream>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void le32(std::vector<uint8_t>& out, size_t offset, int32_t value) {
    for (size_t i = 0; i < 4; ++i) out[offset + i] = uint8_t(uint32_t(value) >> (i * 8));
}
canopy::Point gles_projection(int width, int height, bool left, float x, float y) {
    // Independently exercise the legacy OSD's full matrix chain. This catches
    // incorrect software yaw/slant/pivot or screen-y conventions without GL.
    constexpr float depth = 3.2f;
    const float fh = std::tan(0.785f / 2) * depth, fw = fh * width / height;
    const auto f = canopy::canopy_frame(fw, fh, 0.02f, 1, depth);
    math::Mat4 projection, view, vp, model, neg{}, pos{}, rotation, a, b, m, mvp;
    math::perspective(projection, 0.785f, float(width) / height, 0.1f, 100);
    math::lookAt(view, {0, 0, depth}, {0, 0, 0}, {0, 1, 0});
    math::multiply(vp, view, projection);
    math::rotateY(model, left ? 0.26f : -0.26f);
    neg[0] = neg[5] = neg[10] = neg[15] = 1;
    pos[0] = pos[5] = pos[10] = pos[15] = 1;
    pos[12] = left ? f.LXo(56) : f.RXo(56); pos[13] = f.MY(502);
    neg[12] = -pos[12]; neg[13] = -pos[13];
    math::rotateZ(rotation, left ? canopy::kSlantRad : -canopy::kSlantRad);
    math::multiply(a, neg, rotation); math::multiply(b, a, pos);
    math::multiply(m, b, model); math::multiply(mvp, m, vp);
    const float px = left ? f.LXo(x) : f.RXo(x), py = f.MY(y);
    const float cx = mvp[0] * px + mvp[4] * py + mvp[12];
    const float cy = mvp[1] * px + mvp[5] * py + mvp[13];
    const float cw = mvp[3] * px + mvp[7] * py + mvp[15];
    return {width * (0.5f + 0.5f * cx / cw), height * (0.5f - 0.5f * cy / cw)};
}
}
int main(int argc, char** argv) {
    using namespace kestrel_ui;
    try {
        StockFrame status{0x5a, 1, 0x80, std::vector<uint8_t>(201)};
        status.payload[0] = 1;
        le32(status.payload, 5, -1); le32(status.payload, 9, 1);
        le32(status.payload, 41, 11512); le32(status.payload, 101, 4889);
        status.payload[174] = 0xfe;
        auto wire = stock_packet(status);
        StockParser parser;
        Telemetry t;
        int count = 0;
        auto receive = [&](const StockFrame& f) {
            ++count;
            check(f.sequence == 0x5a && f.flags == 0x80, "Header preservation");
            check(stock_status(f, t, 1000000), "Status decode");
        };
        for (uint8_t b : wire) parser.feed(&b, 1, receive);
        check(count == 1 && parser.buffered() == 0, "Bytewise fragmentation");
        check(t.connected && t.mcs == -1 && !t.link_valid(1000001), "Connected without valid MCS");
        check(t.voltage_mv == 11512 && t.cpu_temperature_x100 == 4889 && t.rf_temperature_raw == -2, "Verified offsets/signed fields");
        check(!t.fresh(3000000), "Telemetry expires without packets");
        check(!t.fresh(999999), "Reject backwards clock");
        auto corrupt = wire; corrupt[2] ^= 1;
        corrupt.insert(corrupt.end(), wire.begin(), wire.end());
        corrupt.insert(corrupt.end(), wire.begin(), wire.end());
        parser.feed(corrupt.data(), corrupt.size(), receive);
        check(count == 3 && parser.rejected >= 1, "Corrupt/coalesced frame resynchronization");
        std::vector<uint8_t> junk(100000, 0x8e);
        parser.feed(junk.data(), junk.size(), receive);
        check(parser.buffered() < 0x5000, "Bounded invalid stream");
        parser.reset();
        check(!stock_status({0, 0, 0, status.payload}, t, 1), "Do not label channel zero as telemetry/MSP");
        auto truncated = status; truncated.payload.pop_back();
        check(!stock_status(truncated, t, 1), "Exact telemetry length");
        auto newer = status; newer.payload.resize(297, 0xa5);
        Telemetry t18;
        check(stock_status(newer, t18, 1000000) && t18.status_payload_bytes == 297 &&
              t18.voltage_mv == 11512 && t18.cpu_temperature_x100 == 4889 && t18.mcs == -1,
              "18.21.10 validated prefix with opaque extension");
        newer.payload.resize(298);
        check(!stock_status(newer, t18, 1), "Do not accept unverified extensions");
        auto ack = stock_packet({0x5a, 1, 0, {}});
        parser.feed(ack.data(), ack.size(), [&](const StockFrame& f) { check(f.payload.empty() && f.flags == 0, "Empty ACK framing"); });

        Model model;
        model.telemetry = t;
        model.tick(1000100);
        check(model.status(1000100) == "LINK UP / NO VALID MCS", "No fabricated streaming state");
        model.telemetry.mcs = 4;
        model.tick(1000200);
        check(model.visual.visible && model.visual.animating && model.visual.rule == 0, "Acquisition start");
        model.tick(1420200);
        check(!model.visual.animating && model.visual.block == 1, "Acquisition settles");
        model.telemetry.connected = false;
        model.tick(1500000);
        model.tick(1740000);
        check(std::abs(model.visual.idle_dim - 0.5f) < 0.001, "Original idle dim timing");
        model.tick(2300000);
        check(model.visual.mark == 1 && !model.visual.animating, "Original idle transition settles");
        model.key(Key::Menu); model.key(Key::Right);
        check(model.menu_open && model.tab == 1, "Menu navigation");
        model.key(Key::Enter);
        check(model.theme == 1, "Local theme action");
        for (int i = 0; i < 40; ++i) model.key(Key::Down);
        check(model.selected < int(model.rows(1000000).size()), "Dynamic row bounds");
        model.key(Key::Back); check(!model.menu_open, "Back closes menu");
        for (float x : {0.0f, 156.0f, 300.0f}) {
            auto l = canopy::project(1920, 1080, true, x, 526);
            auto r = canopy::project(1920, 1080, false, x, 526);
            check(std::abs(l.x + r.x - 1920) < 0.01 && std::abs(l.y - r.y) < 0.01, "Canopy projection mirror symmetry");
        }
        for (auto size : {canopy::Point{1920, 1080}, canopy::Point{1280, 720}})
            for (bool left : {true, false}) for (float x : {-60.0f, 14.0f, 156.0f, 252.0f, 300.0f})
                for (float y : {456.0f, 512.0f, 526.0f, 596.0f}) {
                    const auto p = canopy::project(int(size.x), int(size.y), left, x, y);
                    const auto ref = gles_projection(int(size.x), int(size.y), left, x, y);
                    check(std::abs(p.x - ref.x) < 0.01f && std::abs(p.y - ref.y) < 0.01f, "Software asset geometry matches GLES matrix chain");
                }
        canopy::AlphaSurface pattern{0, 0, 3, 2, {10, 20, 30, 40, 50, 60}};
        auto identity = canopy::warp_alpha(pattern, {{{2, 3}, {5, 3}, {5, 5}, {2, 5}}}, 10, 10);
        check(identity.x == 2 && identity.y == 3 && identity.width == 3 && identity.height == 2 && identity.pixels == pattern.pixels,
              "Identity warp preserves asymmetric texels without a triangle seam");
        auto clipped = canopy::warp_alpha(pattern, {{{-1, 0}, {2, 0}, {2, 2}, {-1, 2}}}, 10, 10);
        check(clipped.width == 2 && clipped.pixels == std::vector<uint8_t>({20, 30, 50, 60}), "Warp clipping preserves source offsets");
        for (bool left : {true, false}) {
            const auto quad = canopy::text_quad(1920, 1080, left, 14, 512, canopy::kTextBig, 96, 80, !left);
            check(quad[1].x > quad[0].x && quad[2].x > quad[3].x, "Right panel text is never mirrored");
            canopy::Homography map(quad);
            for (auto uv : {canopy::Point{0.2f, 0.3f}, canopy::Point{0.5f, 0.5f}, canopy::Point{0.8f, 0.7f}}) {
                const auto p = map.forward(uv.x, uv.y), back = map.inverse(p.x, p.y);
                check(std::abs(back.x - uv.x) < 0.0001 && std::abs(back.y - uv.y) < 0.0001, "Projective mapping round trip");
                const auto f = canopy::screen_frame(1920, 1080);
                float wx = left ? f.LXo(14) : f.RXo(14) - 80 * canopy::kTextBig / 64;
                const auto ref = canopy::project_world(1920, 1080, left, wx + uv.x * 96 * canopy::kTextBig / 64,
                                                      f.MY(512) + (1 - uv.y) * canopy::kTextBig);
                check(std::abs(ref.x - p.x) < 0.01 && std::abs(ref.y - p.y) < 0.01, "Glyph interior follows true 3D perspective, not affine interpolation");
            }
        }
        bool rejected = false;
        try { canopy::Homography invalid({{{0, 0}, {1, 1}, {0, 1}, {1, 0}}}); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Reject folded quads");

        std::string atlas;
        auto put = [&](uint32_t v) { for (int i = 0; i < 4; ++i) atlas += char(v >> (8 * i)); };
        for (uint32_t v : {0x4649554bu, 1u, 40u, 64u, 48u, 2u}) put(v);
        for (uint32_t c : {63u, 65u}) {
            put(c); put(8); put(256);
            std::string pixels(8 * 64, '\0');
            pixels[44 * 8 + 2] = char(255);
            atlas += pixels;
        }
        for (int i = 0; i < 4; ++i) {
            const uint16_t kern = i == 3 ? 0xffc0 : 0; // AA -1 pixel, others zero.
            atlas += char(kern); atlas += char(kern >> 8);
        }
        std::istringstream input(atlas);
        FontAtlas font(input);
        check(font.advance("AA") == 7 && font.advance("A?") == 8, "Baked kerning and fractional advance units");
        auto glyphs = font.rasterize("AA");
        check(glyphs.height == 64 && glyphs.pixels[44 * glyphs.width + 2] == 255 &&
              glyphs.pixels[44 * glyphs.width + 5] == 255, "Glyph rasterization uses the baked baseline and kerning");
        check(font.advance("\xe2\x80\xa2") == 4, "Unavailable Unicode falls back as one glyph, not three bytes");
        rejected = false;
        try { std::istringstream short_input(atlas.substr(0, atlas.size() - 1)); FontAtlas invalid(short_input); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "Reject truncated kerning data");
        if (argc == 3 && std::string(argv[1]) == "--font") {
            FontAtlas real_font(argv[2]);
            check(real_font.advance("AV") < real_font.advance("A") + real_font.advance("V"), "Real Chakra Petch kerning");
            check(real_font.rasterize("VRX 58C \xe2\x80\xa2 11.5V").pixels.size() < 128 * 1024, "Real HUD text stays bounded");
        }
        std::puts("PASS: stock protocol, freshness, shared canopy/GLES geometry, projective rasterization, baked fonts and menus");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
