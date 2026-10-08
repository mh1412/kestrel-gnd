# KestrelUI menu and input controls

## Menu workflow

The standalone controller now implements the original canopy's hierarchy:

1. **Enter or M** wakes the GOGGLE/AIR UNIT picker.
2. **Left/Right** chooses a side; **Enter** unfolds its blade. Up/Down can also
   choose a side for a three-button source.
3. **Left/Right** walks that side's sections; **Up/Down** walks rows.
4. **Enter** on a local setting starts editing; **Up/Down** previews choices.
5. **Enter** applies/saves; **Back/Esc** cancels and restores the original value.
6. Back then folds the blade to the picker, and a further Back closes it.

Each side remembers its last section. Sections are clamped to their own side;
row navigation wraps. Edit transactions are bound to the row's stable ID, so
changing/removing a row cannot turn OK into an unrelated action. A save failure
rolls back the preview and reports the error in the help area.

GOGGLE sections: **HUD, DISPLAY, SYSTEM, INPUT**.
AIR UNIT sections: **RADIO, SERIAL, INFO**.

Editable local preferences:

- Theme, with live preview.
- Canopy detail rows on/off.
- Maximum UI cadence, 1–60 Hz. This never changes camera/HDMI video rate.
- Arrow-key repeat on/off. Menu/OK/Back/Quit are never repeated.

Other rows display verified stock status or explain unavailable capabilities.
RF/camera/recording writes are still backend-owned and are not sent by this UI.

The menu unfolds upward from the wing, fades its content in, and dims the selected
instrument as that blade becomes settings. Editing expands the selected row into
a three-value ladder, with the current value accented. The footer explains the
selected reading/action and how to apply or cancel.

## Input paths

All sources feed the same graphics-independent `Model::key` controller on the UI
thread. There are no per-source copies of menu behavior.

### Keyboard

Stdin supports arrows, WASD in either case, Enter, Menu/M, Esc/Backspace and Q.
The decoder preserves fragmented ANSI/SS3/modifier arrow sequences. A CRLF pair
is one OK event, rather than an edit immediately followed by apply. Standalone
Esc is recognized after a bounded wait. Optional evdev input ignores releases
and applies repeat only to arrows; it does not grab the device exclusively.

### Stock key receiver

Offline inspection of the exact 18.21.10 GlassesUI found a `KeyThread` binding
**127.0.0.1:6222**, reading three-byte messages and forwarding key signals to the
desktop controller. The mapped menu subset is:

```text
packet: [0x40, key_id, event]
key_id: 0 Up, 1 Down, 2 Left, 3 Right, 4 OK, 7 Back
event:  0 normal, 2 arrow-repeat policy
```

Unmapped source types, long/debug events, recording and pairing keys are ignored.
This is enabled with `--stock-keys`. No GPIO exports, directions or electrical
states are changed. GPIO 4/5/6 strings in the UI binary were traced to its one-time
hardware-ID read (`fcmsp_proc.cpp`), not a button poller. GPIO 106 remains owned
by the stock backend; KestrelUI does not take over its pairing behavior.

The mapped packet path was tested with real loopback datagrams. The user also
reported the new controls/menu **worked as expected**. This does not establish
every stock input source or unimplemented record/bind behavior.

### Detached UI and SSH controller

`--control PATH` creates an AF_UNIX datagram socket with owner-only `0600`
permissions. It accepts named UI keys and a read-only `status` request. Packets
are bounded, work per poll is bounded, and no arbitrary backend command is exposed.

The same executable has lightweight client modes:

```sh
kestrel-ui --control PATH --send-key enter
kestrel-ui --control PATH --send-key status
kestrel-ui --control PATH --interactive-control
```

The interactive mode uses the same terminal decoder. **Q exits the client, not
the running UI**; terminal flags/mode are restored on exit. Use `--send-key quit`
only when intentionally ending the UI session and restoring GlassesUI.

An existing live socket is never unlinked by a second UI. A stale socket is
removed only after checking socket type, ownership, refused connection and
unchanged inode. An unrelated file at that path is left intact. Shutdown removes
only the socket inode owned by that instance.

## Local preferences

`--config FILE` reads/writes a small versioned UI-only preference file:

```text
KESTREL_UI_SETTINGS=1
theme=0
fps=25
details=1
repeat=1
```

Unknown headers/values are rejected. Saving uses a same-directory temporary file,
`0600` permissions, checked writes, fsync and atomic rename. Nothing is written
on preview or cancel. Without a config path, applied settings last only for the
session. The test/runtime wrapper uses `ui-settings.conf` in its `/tmp` staging
directory; persistent storage can be chosen explicitly later.

## Validation

- Unit tests: navigation, edit/rollback, stable row IDs, save failure, repeat,
  key normalization, fragmented arrows, CRLF, stock packets and preferences.
- Native and ASan/UBSan rendering regressions: picker, unfolding, expanded edits,
  dynamic values, themes, cache lifetimes and unchanged-text reuse.
- Real-socket tests: receiver framing/reconnect, local IPC ownership, control CLI,
  stock-key datagrams, save/reload and pseudo-terminal client cleanup.
- Device tests: preview EMBER and cancel to KESTREL; apply/save EMBER; restore and
  save KESTREL; preview UI cadence 26 Hz and cancel to 25 Hz; navigate both wings,
  sections and folded/closed states. Preferences were independently read back.
- Hardware decoding advanced **180 frames over approximately three seconds**
  throughout the ongoing menu test, with a reported rate around **60 fps**.

At this validation the UI PID was `3669`, backend/radio were `397`/`396`.
The session remains runtime-only; no vendor configuration/startup was modified.
Use the current `pidof kestrel-ui` rather than relying on historical PIDs.

Current staging paths:

```text
/tmp/kestrel-ui-20261008/control.sock
/tmp/kestrel-ui-20261008/ui-settings.conf
/tmp/kestrel-ui-20261008/run-menu-input.sh
```
