# Stock serial/MSP forwarding: 2026-10-08

The standalone UI previously decoded only channel-1 periodic receiver status.
All channel-0 messages therefore incremented `unknown_frames`, even when their
payloads contained valid flight-controller MSP replies. The SERIAL page exposed
that total as “Unmapped messages”.

## Evidence

Stock firmware: **18.21.10 / Gnd485**. The exact backend inspected has SHA256:

```text
243c11864d266caebdc76d9c4357ca6517fd1afdbbfcbaf9d6c59c113d6d6216
```

A three-second passive loopback capture while the existing UI/video remained
running saved 2,332 packets (7,923,461 pcap bytes), with **zero socket drops**.
Reassembly of backend -> UI TCP 2765 produced 7,337 bytes with no gaps, overlaps
or conflicts. The same raw MSP bytes are visible in the upstream radio-data flow.
The capture and reconstructed streams remain private temporary evidence, not
repository assets. Small non-identifying protocol fixtures are in the tests.

The backend callback beginning at `0x41f628` routes sky message IDs 2 and 4:

- At `0x41f748`, ID 2's first byte becomes **0**, then its payload enters FIFO 1.
- At `0x41f7c8`, ID 4's first byte becomes **1**, then its payload enters FIFO 1.
- `fxn_snd_mspdata_to_gui` at `0x420568` reads FIFO 1 and sends its payload on
  GUI **channel 0**, using `fpv_socket_client_send2gui_no_ack` (`0x448e40`).

The validated envelope is therefore:

```text
stock 8e25 frame, channel 0
  prefix 00 -> forwarded serial byte stream (captured $M> MSP replies)
  prefix 01 -> separate vendor sky notification, contents still opaque
```

The one-byte prefix is removed **per stock envelope**, not per MSP frame. Some
envelopes contain multiple MSP frames; the decoder also retains partial MSP data
across envelopes. Prefix-1 data never enters the MSP parser. Other prefixes and
channels still require their own mapping instead of being guessed as serial.

## Live stream replay

Replaying every stock GUI frame from that capture gave:

```text
REPLAY status=7 unknown=0 outer_bad=0 serial=126 msp=127 status_msp=118 analog=3 malformed=0 vendor=1 variant=BTFL firmware=2026.12.0-alpha arm=0 pack_v=11.54 amps=0.00 remaining=0
```

The MSP frames comprised 118 STATUS replies, three ANALOG replies, three
FC_VARIANT replies and three FC_VERSION replies. `BTFL` identifies Betaflight.
The version reply contains both numeric bytes and a length-prefixed printable
`2026.12.0-alpha` string. The captured ANALOG replies reported roughly 11.5 V pack
voltage and 0.00 A. No cell count was present; no per-cell voltage is invented.

## Implementation

`kestrel_ui/stock_serial.*` is a graphics-independent, read-only adapter:

- Validate inner MSP v1 XOR/native MSP v2 DVB-S2 checksums and direction bytes.
- Bound native-v2 payloads to 4,096 bytes and retained parser storage to 4,105
  bytes; recover from noise, bad checksums and oversized headers.
- Count requests/error replies without interpreting them as measurements.
- Decode FC_VARIANT, FC_VERSION, STATUS/STATUS_EX arm flags and ANALOG pack
  voltage/current/RSSI. Live evidence covers v1; native v2 is regression-tested.
- Treat checksum-valid functions not decoded here as **Other MSP**, rather than
  unmapped stock envelopes. DisplayPort grid decoding remains a follow-up.
- Track STATUS and ANALOG freshness independently. Repeated STATUS or identity
  packets cannot keep old voltage/current alive. Reset partial data and peer
  identity on backend disconnect while retaining diagnostic counters.
- Publish through the existing `BfTelem`/`flight_version` interface. Unchanged
  repeated status does not trigger HUD redraw; a pack reading uses the PACK
  label instead of implying a cell measurement.

The parser never opens the physical UART, polls the FC or issues backend setters.
The only outgoing traffic remains the existing requested stock framing ACK.

SERIAL now exposes FC type/version, frame/byte totals, last MSP function,
STATUS/ANALOG counts, malformed/other MSP, opaque vendor notifications and any
remaining unmapped envelopes. Diagnostics are also emitted in a `SERIAL` log.

## Rendering cost

While investigating, the old SERIAL counters were rebuilding the large menu
snapshot close to every UI tick. Passive menu content is now sampled at **4 Hz**;
button, section, selection and theme changes update immediately. A regression
drives counters at 50 Hz and checks bounded snapshot rebuilds plus immediate
navigation. Reception/decoding runs at the full incoming message rate.

## Checks and receiver validation

Native and ASan/UBSan core, serial and renderer tests passed. Both native and
sanitizer socket/control integration tests passed. Tests cover captured
checksums/layouts, bytewise fragmentation, coalesced frames, corruption recovery,
native v2, signed values, independent expiry, peer reset, bounds and no serial
requests. The entire captured GUI stream also passed replay under sanitizers.

The static Release build was installed through the existing runtime wrapper.
It loaded the user's current **KESTREL theme / 60-Hz cadence** from the staged
preferences. No preference was overwritten. Backend/radio PIDs remained 397/396,
and the independent vendor decoder FPS estimate remained near 60.

Live logs showed:

```text
SERIAL envelopes=3575 bytes=114956 msp=3583 status=3382 analog=67 malformed=0 discarded=0 other=0 requests=0 errors=0 vendor=29 variant="BTFL" firmware="2026.12.0-alpha" active=1 arm=0 pack_v=11.53 amps=0.00
```

Stock `unknown=0 bad=0` throughout that run confirmed the previously unmapped
serial traffic was classified. With SERIAL open, snapshots advanced by about
eight per two-second log interval, matching the bounded refresh policy. Peak
UI RSS was 23,888 KiB. Normal exit verified **all 2,073,600 framebuffer pixels
with zero mismatches**, then entered the restore/relaunch sequence described below.

### Restore/relaunch incident and recovery

During the final stop/stock-restore/relaunch sequence, the display became blank
or frozen and SSH accepted TCP connections without sending a banner. The normal
exit/readback above had already completed. The precise cause of this restore
incident is unresolved; it must not be confused with invalid MSP data, since the
running serial test had zero malformed/unmapped messages.

At the user's direction, Lab Control cycled the OWON SPM6103 (`psu2`) output off
for three seconds and back on, preserving **12 V / 1.5 A** settings. Clean boot
restored SSH and the stock UI. The RAM-only binary/assets were restaged, and the
last observed KESTREL/60-Hz/details-on/repeat-on UI preferences were recreated
after their tmpfs file was lost in the power cycle.

The updated UI was then launched as an ongoing session, with AIR UNIT -> SERIAL
open. UI PID 1477, backend PID 396 and radio PID 395 were observed after recovery.
Fresh logs again reported BTFL / 2026.12.0-alpha, active STATUS/ANALOG decoding,
`unknown=0 bad=0 malformed=0`, and approximately 11.5 V / 0 A / disarmed state.
The independent decoder estimate remained approximately 60 fps. No further
stock restore/reload was performed during this recovered session.

## RC gauge follow-up

The RC gauge originally used only `have_lq/lq_pct`, while the stock MSP adapter
published `have_rssi/rssi_pct`. That left the gauge unfilled despite valid RSSI.
A fresh passive capture contained this checksum-valid ANALOG envelope:

```text
00 24 4d 3e 09 6e 77 00 00 d1 01 00 00 a5 04 61
```

The RSSI field is `0x01d1` = 465 of 1023, rounded to **45%** by the existing
decoder. The capture also contained vendor-batched DisplayPort warning text
(`BATT < FULL`), but no LQ reading. The USB FC was not enumerated during this
follow-up, so the positive RSSI evidence came from the actual radio/GUI feed.

`Model::rc_signal()` now selects fresh, in-range LQ first, otherwise RSSI. It
preserves the metric distinction: the gauge is labeled **RC LQ**, **RC RSSI**,
or **RC --**. A genuine 0% LQ remains authoritative instead of falling back to
a stronger RSSI. Stale, absent and out-of-range values do not fill the bar.
The two gauge rows reserve enough caption width for the longer source label.
SERIAL also shows the selected RC metric and percentage.

Regression checks cover the captured RSSI value, source priority, genuine zero
readings, stale/backwards timestamps and independent ANALOG expiry despite fresh
STATUS. A framebuffer-level test verifies an RSSI-only rung lights at 45% and
returns to its original unfilled color at 0%. Native and ASan/UBSan suites and
socket integration checks passed, including the nonzero RC value through the
real backend adapter and disappearance after disconnect.

The Release build was hot-replaced inside the existing session, keeping the
original stock-restore wrapper paused until the replacement UI eventually exits.
This avoided running the stock framebuffer reload during the upgrade. The old
UI exited with zero framebuffer readback mismatches; backend/radio PIDs stayed
396/395. New UI PID 29445 loaded the unchanged KESTREL/60-Hz preferences and was
left with the menu closed so the RC gauge is visible.

Live logs confirmed `rc_source="RC RSSI"` with changing values of **46–47%**.
A direct read of the visible framebuffer was downloaded and visually checked:
the right RC track is approximately half filled, the **RC RSSI** caption is
readable, and the separate VIDEO track remains intact. This is CPU overlay
readback, not a capture of the underlying video plane. No FC setting, RF command,
serial mode or media configuration was changed.
