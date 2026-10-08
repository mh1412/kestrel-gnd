# KestrelUI: initial standalone software frontend

An independent `kestrel-ui` process for the standard Ascent VRX's stock kernel.
It replaces the display/telemetry role of `GlassesUI` during a bounded test,
while `ar_ldy_gnd` and `daemon_sdio` continue running. The eventual goal is to
connect this frontend to a fully ported Kestrel receiver core through a new
backend adapter.

## Implemented

- LVGL **9.2.2** software rendering, one UI thread, no GL/GBM/DRM/FFmpeg dependency.
- Original canopy ground geometry, slant/perspective projection and color themes.
- Original **Chakra Petch SemiBold** glyphs, kerning, 40px ink/64px cell/baseline 48,
  projected through the same 3D panel transforms as the GLES text quads.
- CPU inverse-homography rasterization with bilinear sampling and cached cropped
  A8 text masks. Glyphs genuinely foreshorten toward the center; right-hand text
  remains readable rather than being mirrored with the panel.
- Original OSD acquired-link sweep and link-loss dim/pulse timing, extracted into
  `ui/canopy.hpp` and also used by the existing GLES implementation.
- Host-baked cropped A8 panel masks; runtime tinting supports all existing themes
  without separate full-color asset sets. At 1080p both masks total 292,678 bytes.
- Two-track canopy gauges, source-derived drone/goggle icons, measured rail gaps
  around state labels and an edge-column menu with the original selection-ring
  styling. Source text sizes and themes are shared with the GLES implementation.
- GOGGLE/AIR UNIT picker, unfolding side menus, section/row navigation, editable
  local UI settings with preview/apply/cancel, contextual help and remembered sections.
- Stdin/evdev keys, validated stock menu packets on loopback UDP 6222, an
  owner-only local control socket, SSH keyboard client and SIGUSR1 menu toggle.
- Nonblocking loopback TCP 2765 server for the stock backend's push connection.
  Bounded frame parsing, checksum validation, reconnect handling and protocol ACKs.
- Verified status layouts: 201-byte payloads from 17.5.3 and 297-byte payloads
  from 18.21.10. Only verified common fields are decoded.
- Two-second telemetry expiry. A connected flag with invalid MCS is distinguished
  from a usable RF link; neither is treated as proof of decoded/displayed video.
- Partial ARGB8888 framebuffer updates respecting stride, channel offsets and
  current page offsets. Visible framebuffer contents are saved/restored on normal
  exit. No modeset, panning, media initialization or module reload in the UI.
- Final framebuffer pixel readback verification and transparent PAM screenshots.

## Current scope

This is a **canopy-inspired bring-up frontend, not full OSD feature parity**.
The monolithic `OSD` class is not linked into this process: its shared geometry,
themes and link transitions have been extracted first. The standalone controller
now follows the original canopy menu flow, but backend-owned actions and all
original menu rows remain to be ported.

Live aircraft telemetry, Betaflight grid/glyph rendering, arm/disarm wipe animations,
reactive motion, gallery/playback and DVR controls are not wired up yet.
Unknown stock notifications/MSP data are counted, not guessed. RF, camera and
recording changes are not sent; the only outbound stock traffic is a framing ACK
when requested. Theme, detail rows, UI cadence and arrow-repeat preferences are
saved only to an explicitly supplied `--config` file; the device wrapper uses a
RAM-staged file and does not change vendor configuration or boot startup.

The receiver currently exposes no `/dev/input` devices. The stock UI's key
receiver is loopback UDP 6222. KestrelUI admits only the mapped source-0x40 menu
keys; hardware GPIO 4-6 were verified as board-ID straps, not buttons. Native
key-source behavior beyond the mapped subset still requires validation.
The prototype supports 1.0x HUD scale; panel masks are generated for a specific
screen resolution, while the font atlas is resolution-independent. Perspective
and typeface now match the original panel geometry; complete GPU effects,
reactive HUD motion and all menu/controller features remain to be ported.
The explicit `--demo` backend supplies synthetic flight readings through the
original `BfTelem` structure to exercise big figures, unit baselines, arm/timer
labels and gauges. The stock backend leaves those fields unavailable.

## Structure

```text
ui/canopy.hpp                 Shared GLES/software geometry and link animation
ui/projective.hpp             Graphics-free inverse projective A8 sampler
kestrel_ui/model.*           Graphics-free UI state and initial local menu
kestrel_ui/input.*           Terminal/evdev/stock keys and local control socket
kestrel_ui/preferences.*     Validated, atomic local UI-preference storage
kestrel_ui/stock_protocol.*  Bounded stock GUI framing and telemetry decoder
kestrel_ui/stock_backend.*   Nonblocking stock GUI server adapter
kestrel_ui/display.*         Offscreen/fbdev CPU overlay presentation
kestrel_ui/view.*            LVGL canopy view and menu
kestrel_ui/font_atlas.*      Baked glyph/kerning loader and text composition
kestrel_ui/perspective_text.* Cached CPU text projection and LVGL presentation
kestrel_ui/assetgen.cpp      Deterministic host-side panel-mask generator
```

No decoded video crosses into the UI process. A future Kestrel IPC adapter can
publish normalized state into `Model` without exposing the stock protocol to the
LVGL view. Operational settings/recording/link recovery belong to the backend;
visual preferences, focus and animation belong to the frontend.

## Host build and preview

Requires CMake, a C/C++17 compiler, Git and network access for the pinned LVGL
checkout. No graphics/media development packages are required. Supply an
existing **9.2.2** checkout with `-DKESTREL_LVGL_SOURCE_DIR=/path/to/lvgl` for
offline builds. Existing `kestrel-gnd` builds remain the default.
The host font-bake/preview tools require Python 3 and Pillow. Use the same
`ChakraPetch-SemiBold.ttf` and accompanying `OFL.txt` shipped with fpvOS/the
fpvOS flasher, or obtain them from the public Chakra Petch/Google Fonts project.
The font and its license are input assets; they are not bundled into this
repository or read by a target font engine.

```sh
cmake -S . -B build-ui -DKESTREL_UI_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-ui -j
build-ui/kestrel_ui/kestrel-ui-assetgen 1920 1080 ui-assets
python3 tools/ui_font_bake.py /path/to/ChakraPetch-SemiBold.ttf ui-assets/canopy-font.kuf
build-ui/kestrel_ui/kestrel-ui --demo --assets ui-assets \
    --show-menu --seconds 5 --screenshot /tmp/kestrel-menu.pam
```

The demo deliberately alternates an artificial RF-link state and supplies
synthetic FC readings to exercise typography/gauges and the shared animation.
It is offscreen unless `--fb` is explicitly supplied. It is
not a telemetry recording or a desktop window. View the screenshot on the host
(the preview helper requires Pillow):

```sh
python3 tools/ui_preview.py /tmp/kestrel-menu.pam /tmp/kestrel-menu.png --checkerboard
```

Omit `--checkerboard` to preserve the transparent overlay in the PNG. The
checkerboard is a visualization of alpha, not captured live video.

## Tests

```sh
ctest --test-dir build-ui --output-on-failure
python3 kestrel_ui/socket_test.py build-ui/kestrel_ui/kestrel-ui ui-assets
python3 kestrel_ui/control_test.py build-ui/kestrel_ui/kestrel-ui ui-assets
build-ui/kestrel_ui/kestrel-ui-tests --font ui-assets/canopy-font.kuf
```

The C++ suite covers bytewise/coalesced framing, checksum corruption,
resynchronization, parser bounds, signed/little-endian status fields, both
verified payload layouts, freshness, link-transition timing and local menus.
It also checks software projection against the original GLES matrix chain at
720p and 1080p without requiring a GPU or graphics libraries.
Projective tests cover asymmetric texture sampling, clipping, mirrored-text
regressions, folded-quad rejection, inverse/forward consistency and glyph interiors
against the 3D projection. Font tests cover metrics, kerning, Unicode fallback and
malformed/truncated atlases.

Register the 240-frame renderer/cache stress test using the generated 1080p assets:

```sh
cmake -S . -B build-ui -DKESTREL_UI_ONLY=ON -DKESTREL_UI_TEST_ASSETS="$PWD/ui-assets"
cmake --build build-ui -j
ctest --test-dir build-ui --output-on-failure
```

It exercises changing flight/receiver values, tab/theme/menu motion, LRU eviction
and reuse of unchanged text. Use the same asset option for the sanitizer build.
The real-socket test covers port exclusion, ACKs, reconnects, malformed/unknown
frames, stdin input and expiry without incoming telemetry. It is host-only.
The control integration test covers picker/section/edit navigation, CRLF,
stock-key decoding, named IPC keys, apply/cancel, preference reload, active/stale
socket ownership and interactive pseudo-terminal restoration. Q exits the
keyboard client without terminating the receiver UI. Both run under ASan/UBSan.

For AddressSanitizer/UBSan:

```sh
cmake -S . -B build-ui-sanitize -DKESTREL_UI_ONLY=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
    -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-ui-sanitize -j
ctest --test-dir build-ui-sanitize --output-on-failure
python3 kestrel_ui/socket_test.py build-ui-sanitize/kestrel_ui/kestrel-ui ui-assets
```

## ARM64 build

Use an AArch64 Linux GCC/G++ toolchain with static C/C++ runtime libraries:

```sh
cmake -S . -B build-ui-arm64 -DKESTREL_UI_ONLY=ON -DKESTREL_UI_STATIC=ON \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ascent-ui-aarch64.cmake
cmake --build build-ui-arm64 -j
aarch64-linux-gnu-strip build-ui-arm64/kestrel_ui/kestrel-ui
file build-ui-arm64/kestrel_ui/kestrel-ui
```

`-DCMAKE_C_COMPILER=...` and `-DCMAKE_CXX_COMPILER=...` can select a custom
toolchain. A relocated/extracted compiler can additionally need `-B` search
paths in **C, CXX and ASM** flags; LVGL includes preprocessed assembly units.
Static linking avoids requiring the stock glibc 2.25/libstdc++ symbol versions.
Generate panel masks using the **host** assetgen executable, not the cross-built
one. Generate 1280x720 masks separately if testing that mode.

## Bounded device replacement

Copy the ARM64 executable, both matching-resolution masks and
the host-generated **`canopy-font.kuf`**, **`canopy-font-OFL.txt`**, and
`test-ascent-ui.sh` into a new `/tmp/kestrel-ui-*` directory using your normal
authenticated device transport. Make the executable runnable. First run an
offscreen check while stock services are still running:

```sh
/tmp/kestrel-ui-test/kestrel-ui --demo --assets /tmp/kestrel-ui-test --seconds 3
```

Then run on the receiver:

```sh
sh /tmp/kestrel-ui-test/test-ascent-ui.sh /tmp/kestrel-ui-test 30
```

The harness:

1. Identifies one stock GUI, its `gui_run.sh` supervisor, backend and radio daemon.
2. Stops the supervisor, terminates only GlassesUI, and waits for exit/zombie state.
3. Runs KestrelUI on TCP 2765 and `/dev/fb0` for 5–120 seconds (or until stopped
   when the duration is explicitly `0`).
4. Saves `ui.log` and `overlay.pam`; verifies framebuffer readback on exit.
5. Checks original radio/backend PIDs and resumes the original supervisor's
   framebuffer reload/relaunch path, recreating GlassesUI.

The wrapper does not edit startup, persistent settings or firmware. Its normal
cleanup and shell error/signal traps restore the stock launcher; this is a test
harness, not a production supervisor. The launcher itself reloads the framebuffer
module during restoration, matching its existing behavior. KestrelUI does not.

TCP 2765 belongs exclusively to one UI. If it cannot be acquired after a bounded
startup retry, the application fails before touching the framebuffer. A demo
framebuffer run does not acquire that stock port: use it only after arranging
exclusive framebuffer ownership. Do not run it over a live GlassesUI writer.

On a manually launched interactive UI, Enter/M wakes the side picker. Left/Right
chooses a wing; Enter unfolds it. In a blade, Left/Right selects sections and
Up/Down selects rows. Enter edits a local setting, Up/Down previews values,
Enter applies/saves, and Back/Esc cancels. Back then folds the blade and closes
the picker. WASD aliases the arrows. Q exits a foreground UI.
`SIGUSR1` toggles the menu, and SIGINT/SIGTERM exit and restore the saved surface.

The detached device wrapper exposes `control.sock` in its staging directory.
Operate it using the same executable without loading graphics in client mode:

```sh
/tmp/kestrel-ui-test/kestrel-ui --control /tmp/kestrel-ui-test/control.sock --send-key status
/tmp/kestrel-ui-test/kestrel-ui --control /tmp/kestrel-ui-test/control.sock --send-key enter
```

Or from the host, using the receiver's normal SSH authentication:

```sh
ssh -t -o HostKeyAlgorithms=+ssh-rsa root@192.168.3.102 \
  '/tmp/kestrel-ui-test/kestrel-ui --control /tmp/kestrel-ui-test/control.sock --interactive-control'
```

Q exits this keyboard client only. See [menu/input behavior and device tests](../docs/kestrel-ui-menu-input.md).

For an ongoing device session, use duration `0` instead of `30`. Launch it from
an attached SSH shell, or detach it with the receiver's `nohup`:

```sh
nohup sh /tmp/kestrel-ui-test/test-ascent-ui.sh /tmp/kestrel-ui-test 0 \
    > /tmp/kestrel-ui-test/session.log 2>&1 < /dev/null &
```

Terminate the `kestrel-ui` process with SIGTERM to finish the session; the wrapper
waits for it to release the framebuffer before restoring GlassesUI. SIGUSR1 can
toggle the menu without ending the session. This remains a runtime-only launch,
not automatic startup after reboot.

## Next milestones

Live HDMI video plus the software HUD has now been confirmed by the user, with
approximately 60-fps stock decoding continuing through the takeover. See the
[live-video follow-up](../docs/kestrel-ui-ascent-test.md#live-video-follow-up).
The CPU perspective version was also confirmed **“Closer and readable”** on HDMI;
see [implementation and device measurements](../docs/kestrel-ui-perspective.md).

1. Measure tearing, latency, VTX reconnect and resolution-change behavior with
   the stock decoder active.
2. Extract more of the original OSD menu/controller and typography without GL
   dependencies, and implement the remaining 2D canopy transitions.
3. Validate stock flight-controller forwarding and add existing MSP/glyph support.
4. Add Ascent physical input and individually validated backend actions/readback.
5. Split the Kestrel core's concrete OSD calls behind state/command interfaces,
   then add the Kestrel IPC adapter and independent Artosyn media/radio backend.
