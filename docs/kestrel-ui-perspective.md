# CPU perspective typography for KestrelUI

## Approach

The canopy is predominantly planar artwork and text transformed through a 3D
camera. KestrelUI now applies those same transforms to glyph quads and samples
them on the CPU, while LVGL handles software composition, invalidation and input.

This preserves genuine perspective foreshortening, panel yaw, slant and baseline
alignment without requiring a software OpenGL driver, depth buffer or CPU video
composition. The original GLES implementation remains the RK3568 frontend.

The settings columns in the current GLES `hud_menu.cpp` are deliberately flat
screen-space elements. The software version follows that design with the same
typeface, edge anchoring, tab underline, accent selection ring and compact rows.

## Matching the original

- **Chakra Petch SemiBold**, matching `OSD::ensure_text_texture`.
- Host-baked 40px ink, 64px texture cells, baseline at 48, left padding at 2,
  16-aligned string surfaces and a kerning table.
- Shared GLES text scales for headlines, units, details, track names and states.
- Model-space glyph quads use `canopy::project_world` and `canopy::text_quad`.
  Right-aligned text subtracts its advance in model space; glyphs are not mirrored.
- The inverse homography maps destination pixel centers back to a continuous
  texture plane. Bilinear sampling avoids holes and a diagonal triangle seam.
- Two tracks per wing, tapered blocks, measured state-label gaps and matching
  drone/goggle icon paths, baked into spare glyph slots on the host.
- Mixed-size headline/unit text shares the original baseline; digit-width
  measurement uses zero references so adjacent units do not shuffle with readings.
- Unavailable FC values remain unavailable on the stock adapter. `--demo` alone
  supplies synthetic `BfTelem` values to show the complete typography/gauges.

The generated font atlas is **243,022 bytes**, with 99 glyphs including ASCII,
degree, bullet and two icons. Ship its accompanying `canopy-font-OFL.txt`.
The source TTF and font engine are only needed on the host.

## Lifetime and caching

Projected A8 masks are cached by string and quad coordinates. Color/opacity are
applied at draw time, so themes and link fades reuse the same masks. Menu movement
translates cached tiles rather than rerasterizing text at every intermediate position.

The LRU is trimmed to 96 entries or 1 MiB **before** each UI tick. New tiles can
briefly exceed that soft limit during a draw; the renderer stress test checks a
1.5 MiB frame budget. Eviction never occurs inside a draw callback, where queued
LVGL tasks could still reference the image. LVGL image/header cache entries are
dropped before releasing a tile.

The sampler uses incremental scanlines and one reciprocal per pixel on cache
misses. Unchanged strings incur no projection/rasterization work. Whole video
frames stay in the vendor hardware decoder/display path.

## Host verification

The graphics-free suite verifies:

- 720p/1080p software geometry against the existing GLES matrix chain.
- Text-quad interiors against the 3D plane, not just its corners.
- Forward/inverse mapping, asymmetric identity sampling and clipping.
- Right-wing text orientation and folded/degenerate quad rejection.
- Font metrics, kerning, baseline placement, Unicode fallback and truncated files.

A 240-frame LVGL regression exercises changing readings, arm/timer labels,
menu sides, selections, themes, animation, cache eviction and unchanged-text reuse.
Both suites pass with AddressSanitizer/UndefinedBehaviorSanitizer. The existing
real-socket integration suite also passes.

## Ascent validation

Tested on the same stock 18.21.10 receiver with live video. The first prototype
was stopped gracefully; the improved executable and font assets were deployed
in RAM. Stock RF/video backend processes were retained.

In a bounded **20-second**, 1080p test with the diagnostic menu open:

| Measurement | Result |
| --- | --- |
| Real stock status updates | 49 |
| Invalid protocol frames | 0 |
| Peak RSS | 19,696 KiB (approximately 19.2 MiB) |
| Projected text masks at steady state | Approximately 200 KiB, 35 rasterizations |
| UI loop ticks | 477, configured maximum 25 Hz |
| Framebuffer readback | 2,073,600 pixels; zero mismatches |
| Radio/backend ownership | Original processes retained |

The logged render average approached **1.9 ms per UI tick**, including cold
startup and idle ticks; it is not a full-screen software-3D benchmark. The explicit
demo with continually changing simulated values is a heavier workload than the
current stock HUD. Complete 25/60-fps animation/FC-feed performance is not inferred
from these steady-state measurements.

After the bounded test, the improved UI was left running with its menu hidden.
The hardware decoder advanced **180 frames in approximately three seconds**, with
a reported rate around 60 fps. The user confirmed **“Closer and readable”** on HDMI
over live video.

At that validation the UI PID was `22481`; backend/radio PIDs were `397`/`396`.
PIDs are session-specific. The runtime launcher is
`/tmp/kestrel-ui-20261008/test-ascent-ui-perspective.sh`, and its session log is
`/tmp/kestrel-ui-20261008/session-perspective.log`. The old executable was retained
as `kestrel-ui-flat-backup`; startup configuration was not changed.

## Remaining fidelity work

Reactive pitch/roll motion, arm/disarm wipe timing, background warp/shader effects,
gallery transitions, all original menu/actions and live FC/MSP/glyph input remain
to be integrated. The CPU text/plane projection is a reusable basis for those
effects, rather than an assertion of complete visual/behavioral parity today.
