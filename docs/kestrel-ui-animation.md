# Ascent CPU menu animation: 2026-10-08

Tested on the standard Ascent VRX, stock 18.21.10/Gnd485 firmware, dual
Cortex-A53 at 800 MHz (`performance` governor), 1920x1080 ARGB framebuffer.
KestrelUI remains an LVGL 9.2.2 software renderer. The tests below ran with an
active RF link (MCS 10) and the vendor decoder reporting approximately 60 fps.

## Changes

- Build ARM64 with `Release`, rather than `MinSizeRel`.
- Increase the partial draw buffer from 48 to 192 rows (~1.4 MiB at 1080p),
  reducing repeated custom draw callbacks. Keep the retained screenshot surface
  and framebuffer backup; no extra full-screen LVGL drawing buffer is allocated.
- Preserve absolute frame phase and skip expired deadlines. Explicitly refresh
  the current pose instead of relying solely on LVGL's independent timer phase.
- Pre-render the likely blade in the picker, and update its content cache when
  the fully opened menu changes. Freeze that image while moving. Drop LVGL image
  caches before destroying or replacing its draw buffer.
- Use an opaque XRGB8888 menu snapshot (~2.47 MiB at 1080p), allowing row copies
  rather than a source-alpha blend for every pixel. The settled menu is also
  opaque: returning to the previous 94% opacity caused a final-keyframe spike.
- Animate the reveal of the stationary menu image. Repaint only the symmetric
  difference of successive reveal rectangles; unchanged pixels remain intact.
- Hide the selected HUD wing during motion and restore it when closure finishes.
  Reveal the picker after folding. Restrict the picker object's damage to its
  two label rows rather than both complete HUD wings.
- Log animation-specific average/p95/max costs, completed paints and snapshot
  builds. Keep the existing all-tick average, but do not use it as animation FPS.

## Offscreen receiver comparison

Both binaries used `Release`, 192-row buffers, 50 Hz absolute pacing, matching
assets and the same live receiver workload. Each run lasts six seconds;
animation costs exclude the first second. These are CPU rendering measurements,
**not framebuffer/HDMI frame times**. CPU use is a percentage of one core.

| Variant | Average | p95 | Maximum | Skipped deadlines | CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Earlier direct-drawing smoothing build | 21.32 ms | 55.77 ms | 80.75 ms | 93 | 38.93% | 12,844 KiB |
| Cached plane, edge strips, bounded picker damage | 4.48 ms | 24.40 ms | 68.41 ms | 41 | 13.99% | 15,420 KiB |

The final run built one snapshot. It counted 169 animated ticks and 168 paints;
a tick whose quantized reveal produces no damage need not flush. Small tests
vary with vendor workload; the result does not establish a guaranteed 50-fps UI.

Intermediate approaches were measured and replaced: an ARGB snapshot retained
expensive per-pixel alpha work, and repainting the complete cached plane still
caused excessive memory traffic. Edge strips reduced that traffic, while keeping
the blade opaque and shrinking picker damage reduced open/fold keyframe costs.

## On-screen validation

The final static executable was staged in `/tmp/kestrel-ui-20261008/kestrel-ui`.
The runtime replacement wrapper preserved the stock backend/radio processes
(PIDs 397/396 during this test), stopped the GlassesUI supervisor, and restored
GlassesUI on normal exit. Saved preferences remained VIOLET, 50 Hz, details on,
arrow repeats on.

After 16 GOGGLE unfold/fold cycles and four AIR UNIT cycles, the live UI logged:

```text
ANIMATION ticks=481 painted=481 recent_avg_us=6411 recent_p95_us=32897 recent_max_us=122191
```

These costs include CPU framebuffer presentation and cover the latest 256
animation ticks. Peak UI RSS was 23,656 KiB. All 481 counted ticks painted.
Decoder frame counts advanced from `0x000b9f54` to `0x000ba3af` (1,115 frames)
through the approximately 19-second exercise; its independent FPS field stayed
near 60. The exact backend binary's validated read-only stats offsets were used,
as described in the earlier [device test](kestrel-ui-ascent-test.md).

Normal termination verified all 2,073,600 visible framebuffer pixels with **zero
mismatches**, saved the overlay, and restored GlassesUI. The downloaded menu
preview was visually checked for glyphs, values, layout, opaque backing and the
opposite perspective HUD wing. The verified build was then relaunched as an
ongoing runtime-only session with the same saved preferences.

## Regression checks and limits

- Native core/render tests and control/stock-socket integration checks passed.
- ASan/UBSan core/render and both integration checks passed.
- Rendering stress still covers 240 changing telemetry/menu/theme frames.
- Added checks for hidden picker prewarming, correctly positioned opaque menu
  pixels on both sides, no telemetry-triggered snapshot rebuild during motion,
  and preserving the outgoing cache during closure.
- Incremental open/close frames are compared against full redraws. RGB contents
  under fully transparent pixels are ignored; all visible/alpha values must match.
- No GPU/GE2D rendering, framebuffer panning, vsync synchronization or media
  configuration changes were introduced.

The average animation cost is substantially lower, but the 32.9-ms on-screen
p95 still exceeds a 50-Hz frame budget. The 122-ms outlier shows cold/menu-side
work remains visible. First-time preparation, telemetry changes and vendor
scheduling still need profiling. HDMI tearing and perceived smoothness require
user observation; rendering cost and decoder FPS alone cannot establish them.
