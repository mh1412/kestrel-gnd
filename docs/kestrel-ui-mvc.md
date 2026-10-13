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

## HUD handoff contract

Local message-boundary IPC carries setup, display-ready snapshots, ready frames
and buffer releases. Pixels use bounded shared-memory buffers. Explicit leases
prevent a producer from overwriting a frame the UI is rendering. The UI applies
scene visibility independently, drops a disconnected HUD's layer, and can restart
the renderer without changing radio/video or reloading the framebuffer driver.

Rendering surfaces describe bounds, pixel format, stride and alpha semantics;
their layout must be validated and not assume a universal full-screen allocation.
Hardware GPU implementations can later use buffer-handle/fence handoff behind
the same process ownership boundaries.

## Second checkpoint: passive renderer process

`kestrel-hud` is a separate executable containing only the passive LVGL View,
font/theme resources, clock and IPC/buffer support. It has no backend parser,
input implementation, framebuffer mapping or media initialization. The UI starts
the selected executable (sibling `kestrel-hud` by default); `--hud-executable`
allows a compatible renderer to be supplied explicitly. Builds select the
implemented backend with `KESTREL_UI_RENDERER=lvgl`; a GPU adapter remains future
work, rather than an advertised working renderer.

The default Canopy layout exports three cropped regions: two lower HUD wings and
the central status area. At 1080p the two shared slots total **7,188,480 bytes**
(about 6.86 MiB), not two whole 1080p images. Layouts can describe up to eight
bounded regions, including a generic HUD region; the compositor validates the
viewport, role, count and pixel budget. Each renderer uses a 512-KiB projected
text-cache budget so splitting processes does not double the old aggregate cache.

The local socket pair uses `SOCK_SEQPACKET`, is private to the UI/child, and is
passed as an inherited close-on-exec exception. Its wire header is `KHUD`, version
2, message type and 64-bit sequence, with explicitly little-endian fields.
Strings and fractions have bounds/finite-value checks; raw C++ object layouts
and pointers never cross the boundary. Messages are:

- **Hello:** renderer-proposed cropped layout.
- **Setup:** UI-validated ARGB8888/straight-alpha layout and two leased slots,
  with one shared-memory descriptor passed using `SCM_RIGHTS`.
- **Data:** a complete display-ready snapshot. Unchanged data is not resent;
  the renderer coalesces queued snapshots instead of drawing expired poses.
- **Frame:** slot ID, data sequence, changed-region mask and rendering/cache statistics.
- **Release:** exact slot/sequence lease acknowledgement.

The UI holds its front slot until its synchronous drawing work has finished.
It drops image-decoder cache references before releasing or replacing a slot.
The producer cannot reuse a leased slot. Buffer capacity is sealed on memfd-capable
kernels; an unlinked owner-only file is the compatibility fallback.

HUD data preparation remains in the UI Controller. Gallery mode supplies one
disabled snapshot and excludes HUD objects immediately, independently of renderer
latency. Changes to hidden telemetry do not trigger drawing. The inactive renderer
still has its private IPC endpoint ready for the next enabled snapshot.

Renderer disconnection clears the layer and triggers a bounded restart/resync;
a stalled outstanding request also triggers recovery. The UI's input, menu edit
state and preference callbacks remain alive. Parent death signals and normal
UI shutdown stop/reap the child. Restarting the HUD does not reload the framebuffer
driver or touch the receiver backend.

Verification includes exact pixel comparison of shared worker output against
the synchronous passive View, malformed schema/geometry checks, descriptor flags,
shared-memory visibility, gallery transparency/pause, forced renderer death and
resynchronization. Real UI control tests kill the HUD while navigating and verify
the menu selection and applied theme survive. Socket tests audit that the worker
has one IPC socket, no physical input/framebuffer descriptors, and no inherited
backend connection. Native and ASan/UBSan suites passed. An initial static ARM64
offscreen run on the VRX also produced HUD frames, normal UI/child exit and left
stock GUI/backend/radio PIDs unchanged; framebuffer/device validation follows.

## Device checkpoint: UI/HUD composition and recovery

The matched static Release executables are staged in `/tmp/kestrel-ui-dev` and
running with the recovered KESTREL theme, 60-Hz cadence, details and repeat on.
The stock backend/radio remain PIDs 397/396. After the user powered the paired
VTX/FC, receiver status showed connected/MCS 10 and valid stock STATUS/ANALOG
telemetry. The user confirmed **“Video and UI look correct.”**

Descriptor audit established:

- UI owns `/dev/fb0`, backend connection, menu-control and stock-key endpoints.
- HUD has `/dev/null` input, inherited diagnostic output, one private IPC socket
  and `/memfd:kestrel-hud`; no framebuffer, UART, GPIO or backend socket.

During a theme preview the HUD alone was killed with SIGKILL. Cancel still
restored KESTREL, section navigation continued, and menu state stayed in the UI.
UI PID 27414 survived while renderer PID 27418 was replaced by 28382. The renderer
received current data after restart; `hud_restarts=1` was visible over the existing
control endpoint. Backend/radio PIDs did not change. The decoder counter advanced
208 frames during the approximately 3.5-second exercise and its FPS estimate
remained near 60.

### Composition optimization

Initial device measurements showed unnecessary repainting of both HUD wings
when only one region changed. Protocol version 2 adds an explicit region-damage
mask. The producer compares the completed region pixels with its previous frame;
the UI invalidates only changed regions, retaining exact slot leases and latest-
state coalescing. A regression verifies an RSSI-only change damages just the right
wing and still exactly matches the synchronous reference pixels.

The pair was updated in-place while the original stock-restoration wrapper was
paused, avoiding a stock framebuffer-module reload during the upgrade. The old
UI exited with **2,073,600 pixels / zero readback mismatches**. The new UI/HUD pair
was left running. An older UI zombie is retained by that paused wrapper until
the replacement session finishes; diagnostics should use `hud_pid`'s PPid or the
recorded active UI PID rather than treating every `pidof kestrel-ui` result as a
live writer.

A visible-framebuffer read was downloaded and checked after renderer recovery:
the same Canopy typography/geometry, real pack/current/disarmed readings, RC
source caption and video ladder were present, with no duplicated or stale layer.
The preview composites the ARGB overlay onto a host background; live video output
was separately confirmed by the user.

### Measurements

Eight-second closed-menu/live-video sample:

| Metric | UI | HUD |
| --- | ---: | ---: |
| CPU, percentage of one core | 6.48% | 1.62% |
| Current RSS | 26,984 KiB | 10,104 KiB |
| Peak RSS | 29,496 KiB | 10,104 KiB |
| Proportional memory (PSS) | 26,126 KiB | 7,370 KiB |

Combined UI/HUD CPU is **4.05% of the two-core chip**; combined PSS is 33,496 KiB
(about **32.7 MiB**). PSS avoids counting shared pages twice. Video/backend CPU is
not included in these UI/HUD percentages.

After eight GOGGLE and four AIR UNIT open/fold cycles:

```text
ANIMATION ticks=356 painted=355 recent_avg_us=6152 recent_p95_us=38418 recent_max_us=78451
```

These are parent UI update/presentation costs, including framebuffer writes,
not the HUD worker's independent render time or guaranteed display FPS. The
last quantized reveal may produce no damage. The 38-ms p95 still exceeds the
60-Hz budget; long keyframes/cold work remain. The separate HUD coalesces updates
and changes frame only when needed; it is not forced to render a 60-Hz full image.

Latest native/sanitizer schema, core, renderer and worker suites plus real
socket/control recovery checks passed. Gallery pause/visibility is verified with
real renderer buffers in those tests; full gallery/media UI and GPU rendering
remain subsequent adapters rather than completed standalone features.
