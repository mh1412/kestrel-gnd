# Kestrel UI / passive HUD architecture

Development branch: `dev`, based on the verified standalone Ascent UI.

## Process ownership

- **Receiver backend:** radio, validated telemetry and operational commands,
  video decode, recording/playback. During migration this remains the stock
  `ar_ldy_gnd`; its read-only adapter publishes `BackendState`. Replacing it with
  the fully ported `kestrel-gnd` is a separate media/radio milestone.
- **kestrel-ui:** MVC controller/model, input, preferences, menus/gallery and
  final overlay composition. Sole owner of `/dev/fb0` on Ascent.
- **kestrel-hud:** passive MVC renderer of supplied display-ready `HudData`.
  Receives data/configuration from the UI and returns a rendering surface.
  It does not parse MSP, query the FC or own the framebuffer.

Hardware-specific builds select the rendering implementation. The first is
LVGL 9.2.2/software; the contracts contain no LVGL, EGL/GLES or vendor handles.

## MVC boundaries

`Model` stores backend-reported and local UI state and exposes read-only menu,
status and RC presentation queries. `Controller` owns input actions, preview /
apply / cancel transactions, persistence callbacks, scene changes and preparing
HUD data. `View` reads the model; prewarming a menu uses a derived preview copy
instead of changing controller state.

`BackendState` separates VRX state, VTX identity (unreported until validated), FC
telemetry and serial diagnostics. Stock adapters accept only that state, so they
cannot alter UI preferences or navigation. FC firmware remains FC firmware; it
is never relabeled as VTX identity.

`HudData` contains display-ready text, units, validity/visibility, interpreted
gauge levels and presentation values. Freshness, RSSI/LQ selection, MCS ladder,
voltage/current formatting and the existing cell-voltage drop calculation are
prepared upstream. A HUD renderer places/draws the given values without another
telemetry or command path.

Scene policy:

| Scene | HUD |
| --- | --- |
| Live | Visible |
| Menu | Visible where uncovered |
| GalleryBrowse | Hidden |
| GalleryPlayback | Hidden |

Hiding the HUD does not stop telemetry reception. These scene policy hooks do
not themselves implement standalone gallery listing or video playback; those
features still require the receiver/media command adapter.

## First checkpoint: controller/state separation

- Moved UI actions, persistence callbacks and link presentation preparation into
  `controller.*`; input routes through the controller.
- Moved backend data into `backend_state.hpp` and made stock adapters independent
  of the UI model.
- Extracted `BfTelem` into `ui/flight_telemetry.hpp`, preserving its existing
  field layout and legacy parser API without importing `MspOsd` into UI contracts.
- Added display-ready passive HUD data and gallery visibility policy.
- Made the View model reference const and removed snapshot-time model mutations.

Native and ASan/UBSan core, serial and renderer tests plus real socket/control
integration checks passed. The static Release build also ran a three-second
offscreen demo on the connected 18.21.10 VRX: 72 ticks, normal exit, peak RSS
15,388 KiB. Stock GUI/backend/radio PIDs remained 630/397/396. This first device
check did not take over the framebuffer; the process handoff is the next stage.

## HUD handoff contract (next stage)

Local message-boundary IPC carries setup, display-ready snapshots, ready frames
and buffer releases. Pixels use bounded shared-memory buffers. Explicit leases
prevent a producer from overwriting a frame the UI is rendering. The UI applies
scene visibility independently, drops a disconnected HUD's layer, and can restart
the renderer without changing radio/video or reloading the framebuffer driver.

Rendering surfaces describe bounds, pixel format, stride and alpha semantics;
their layout must be validated and not assume a universal full-screen allocation.
Hardware GPU implementations can later use buffer-handle/fence handoff behind
the same process ownership boundaries.
