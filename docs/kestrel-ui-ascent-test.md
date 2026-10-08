# KestrelUI Ascent bring-up — 2026-10-08

## Result

The initial standalone LVGL software frontend was cross-built, executed on the
standard Ascent VRX, and temporarily replaced GlassesUI while the stock backend
and radio daemon remained running. Final framebuffer readback matched every
rendered pixel, and real stock receiver status was decoded.

The receiver was on **18.21.10**, not the earlier 17.5.3 architecture snapshot.
It reported **no RF link** during these tests. This proves UI execution,
framebuffer writes, telemetry integration and stock-UI restoration; it does
**not** establish live-video coexistence, externally observed HDMI alpha/tearing,
complete GUI-initialization compatibility or full Kestrel feature parity.

**Live-video follow-up:** after the user updated the VTX firmware and confirmed
stock video, KestrelUI was launched over the working stream. Decoding continued
at approximately 60 fps, and the user confirmed **live video plus the canopy HUD
on HDMI**. Details are recorded below; the initial no-link measurements remain
separate from this later validation.

## Environment and artifacts

- AArch64 stock receiver; `/etc/app.version`: 18.21.10, Gnd485, build 202608141800.
- LVGL 9.2.2 pinned commit `7f07a129e8d77f4984fff8e623fd5be18ff42e74`.
- GCC/G++ 13 cross-build, static ARM64 executable, stripped size approximately
  2.13 MiB. No dynamic section or GLES/EGL/GBM/DRM dependencies.
- Two host-generated 1080p alpha masks, 517x283 each, total 292,678 bytes with headers.
- Runtime-only staging: `/tmp/kestrel-ui-20261008`.
- Host build/test artifacts: `/tmp/opencode/kestrel-ui-{host,arm64,sanitize}`.
- Generated masks: `/tmp/opencode/kestrel-ui-assets`.

No credentials, private peer identifiers or vendor binaries are included here.
The existing access/startup configuration was not modified.

## Protocol difference found and resolved

The first 30-second UI takeover received correctly framed periodic packets,
but rejected their status layout: **297 bytes**, instead of the documented
17.5.3 layout's **201 bytes**. GlassesUI was restored after that test.

The downloaded exact running 18.21.10 backend had SHA-256:

```text
243c11864d266caebdc76d9c4357ca6517fd1afdbbfcbaf9d6c59c113d6d6216
```

Offline inspection of `Fxn_period_msg_to_gui` at `0x41b6bc` established:

- Zeroes a 296-byte structure at stack offset `0xe0`.
- Prefixes subtype 1 and sends 297 bytes on GUI subchannel 1.
- Preserves the following offsets relative to the structure after the subtype:

| Field | Offset | Evidence |
| --- | --- | --- |
| Quality grade (raw) | 0 | Stores at stack `0xe0` |
| MCS | 4 | Stores at `0xe4` |
| Connected | 8 | `fpv_bb_is_connect`/relay result stored at `0xe8` |
| Frequency (raw) | 12 | `fpv_bb_get_loacl_freq_by_index` result at `0xec` |
| Distance (raw) | 16 | `fpv_bb_get_distance` result at `0xf0` |
| Receiver voltage, mV | 40 | Fitted voltage result at `0x108` |
| CPU temperature x100 | 100 | `g_cpu_temp_x100` at `0x144` |
| RF temperature (raw signed byte) | 173 | `fpv_bb_get_rf_board_temp` result at `0x18d` |

The parser accepts only the two verified payload lengths. New extension fields
and other notifications remain opaque. A subsequent 30-second run decoded
**74 status updates**, with zero rejected/unmapped frames.

## Final 20-second run

After eliminating redraws caused solely by repeated identical status packets:

| Measurement | Result |
| --- | --- |
| Framebuffer | 1920x1080, 32 bpp, stride 7680, current offset 0,0 |
| Channel offsets | R16, G8, B0, A24 |
| Mapping | 24,883,200 bytes; existing three-height stock allocation |
| Real status updates | **49** |
| Unknown/rejected frames | **0 / 0** |
| Reported RF state | Disconnected; MCS raw 0 correctly treated as invalid |
| Receiver voltage | **11,352 mV** |
| CPU temperature | Approximately **48.36–48.89 C** |
| Peak RSS | **19,352 KiB** (about 18.9 MiB) |
| UI loop ticks | **480** at a configured maximum of 25 Hz |
| Partial-buffer flushes | **68**, including initial screen and menu animation |
| Framebuffer pixel readback | **2,073,600 pixels, zero mismatches** |
| Backend/radio PIDs | Unchanged throughout the takeover |
| Stock GUI | Relaunched successfully by its original supervisor |

The logged render-time average fell below 1 ms per UI tick after the initial
redraw/animation. This is an average including idle ticks, **not** a full-screen
animation benchmark or a claim of 1080p video performance. Steady temperature
changes produced small text-region flushes rather than full canopy redraws.

One intermediate takeover failed with `EADDRINUSE` before framebuffer access;
the wrapper restored GlassesUI. Startup now includes a bounded address-acquisition
retry, and the final run completed successfully. Failure logs are printed before
restoration rather than hidden by the shell's early exit.

## Host verification

- CTest graphics-free core suite passed.
- Software asset coordinates matched the existing GLES matrix chain at 720p and
  1080p, including panel pivots, slant, perspective and screen-y convention.
- Real-socket integration passed: port conflict, fragmented/coalesced packets,
  protocol ACK, corruption, unknown channel, reconnect, stdin input and expiry.
- Core and socket tests passed with AddressSanitizer and UndefinedBehaviorSanitizer.
- Transparent host preview and device overlay capture had alpha range 0–255.
- ARM64 offscreen execution passed before taking over the framebuffer.
- Shell syntax and Git whitespace checks passed.

The full RK3568 GLES receiver was not rebuilt in this environment; its graphics,
media and vendor-client build dependencies are not staged here. The extraction
keeps its existing canopy state members and delegates geometry/transition math
to the shared header, rather than changing its presentation path.

The final device overlay and host-converted preview were retrieved as:

```text
/tmp/opencode/kestrel-ui-host/ascent-final.pam
/tmp/opencode/kestrel-ui-host/ascent-final.png
/tmp/opencode/kestrel-ui-host/ascent-final.log
```

The PNG preview composites the transparent capture over a checkerboard; it is
not an HDMI/video capture.

## Repeating the device test

While this boot's staging directory exists, run in the receiver's SSH shell:

```sh
sh /tmp/kestrel-ui-20261008/test-ascent-ui.sh /tmp/kestrel-ui-20261008 30
```

It restores GlassesUI automatically afterward. Logs and final transparent overlay
are in that directory. For the next live-video test, have a paired VTX streaming
and observe HDMI while the takeover runs; process liveness/readback alone cannot
establish that the displayed video remained correct.

## Live-video follow-up

### No-picture diagnosis and working baseline

After the VTX was powered on during the first ongoing UI session, the stock
periodic feed reported an RF connection with MCS values around 6–10, but:

- `g_vdec_stats` remained zero; no decoded-frame progress was observed.
- A passive loopback capture contained no video-message header candidates and
  no large compressed-video flow.
- Restoring GlassesUI did not restore the picture; the user confirmed it was
  still missing with the stock UI.
- A normal receiver reboot and user VTX power-cycle also left stock decoding at
  zero. The user had not established a video baseline after the receiver update.
- The stock camera-initialization pending flag remained set, and the air-unit
  encoder-running status flag remained clear.

The user then updated the **VTX firmware** and reported stock video restored.
The previous VTX version was not established, so the exact compatibility failure
is not claimed. No firmware was flashed by the KestrelUI implementation. The
receiver remained on 18.21.10, with the same backend executable hash.

### Decoder and HDMI validation

Read-only statistics were located in the exact non-PIE 18.21.10 backend:
`g_vdec_stats` at `0x6ac060`, decoded-frame counter at `+0x68`, float FPS estimate
at `+0x6c`. Offline inspection of `fpv_vdec_statistic_update` confirmed those
fields before sampling; no debugger attach, process pause or memory write was
used.

| Phase | Counter samples | Result |
| --- | --- | --- |
| Stock UI, after VTX update | `0x341d` → `0x34d1` | **180 decoded frames** over approximately 3 seconds |
| KestrelUI active | `0x4ac7` → `0x4b7c` | **181 decoded frames** over approximately 3 seconds |

The decoder FPS estimate remained approximately **60 fps**. The air-unit
running flags changed to `0x03`, including the encoder-running bit. Radio and
backend process IDs were unchanged during the KestrelUI takeover.

KestrelUI's diagnostic menu was closed with SIGUSR1. The user then explicitly
confirmed **“Video and HUD work”** on HDMI. This establishes practical live-video
and software-overlay coexistence for this session, rather than inferring it
from process liveness or a framebuffer screenshot alone.

The live stream also generated unmapped subchannel-0 notifications. These are
counted but still not decoded as MSP/flight-controller telemetry. Frame checksums
remained valid; peak UI RSS was approximately **19 MiB**.

### Ongoing session

KestrelUI was left running with the menu hidden, under the runtime wrapper with
duration `0`. At validation:

```text
UI PID:      20106
Backend PID: 397
Radio PID:   396
UI log:      /tmp/kestrel-ui-20261008/ui.log
Session log: /tmp/kestrel-ui-20261008/session-live-video.log
```

PIDs are session-specific. SIGUSR1 toggles the menu; SIGTERM ends the UI session
and lets the wrapper restore GlassesUI. No persistent startup change was made.
VTX disconnect/reconnect, resolution changes, physical controls and full OSD
feature parity remain separate follow-up tests.
