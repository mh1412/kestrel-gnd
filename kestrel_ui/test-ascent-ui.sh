#!/bin/sh
# Runtime-only UI replacement (bounded by default; 0 runs until stopped).
# Run on the Ascent after copying the
# binary/assets into a new /tmp directory. Never edits startup or RF settings.
set -eu
directory=${1:?Usage: sh test-ascent-ui.sh /tmp/kestrel-ui-test [seconds]}
seconds=${2:-30}
case "$directory" in /tmp/kestrel-ui-*) ;; *) echo 'Use a dedicated /tmp/kestrel-ui-* directory' >&2; exit 1;; esac
case "$seconds" in ''|*[!0-9]*) exit 1;; esac
if [ "$seconds" -ne 0 ]; then
    [ "$seconds" -ge 5 ] && [ "$seconds" -le 120 ]
fi
[ -x "$directory/kestrel-ui" ]
[ -f "$directory/canopy-left.kui" ] && [ -f "$directory/canopy-right.kui" ]
gui=$(pidof GlassesUI)
backend=$(pidof ar_ldy_gnd)
radio=$(pidof daemon_sdio)
# Reject absent/ambiguous instances rather than signal an arbitrary process.
case "$gui:$backend:$radio" in *' '*|::*|:*|*:) echo 'Expected one stock UI, backend and daemon' >&2; exit 1;; esac
[ -n "$gui" ] && [ -n "$backend" ] && [ -n "$radio" ]
parent=$(awk '/^PPid:/ {print $2}' "/proc/$gui/status")
case "$parent" in ''|0|1) echo 'Cannot identify GUI supervisor' >&2; exit 1;; esac
tr '\000' ' ' < "/proc/$parent/cmdline" | grep -q 'gui_run.sh' || exit 1
mode=$(cat /sys/class/graphics/fb0/virtual_size)
case "$mode" in 1920,*) flag=/tmp/flag_1080;; 1280,*) flag=/tmp/flag_720;; *) echo 'Unknown stock UI mode' >&2; exit 1;; esac
restored=0
ui_pid=0
cleanup() {
    [ "$restored" -eq 0 ] || return
    restored=1
    # Stop the overlay writer before handing the framebuffer back, including
    # when the session wrapper itself receives a termination signal.
    if [ "$ui_pid" -ne 0 ]; then
        kill -TERM "$ui_pid" 2>/dev/null || true
        wait "$ui_pid" 2>/dev/null || true
        ui_pid=0
    fi
    # Its child is gone, so the original supervisor resumes its existing
    # framebuffer reload/relaunch path. Leave it running after this test.
    touch "$flag"
    kill -CONT "$parent" 2>/dev/null || true
    sleep 3
    echo "RESTORED_GUI=$(pidof GlassesUI || true)"
    echo "BACKEND=$(pidof ar_ldy_gnd || true) RADIO=$(pidof daemon_sdio || true)"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
kill -STOP "$parent"
kill -TERM "$gui"
for i in 1 2 3 4 5; do
    [ ! -d "/proc/$gui" ] && break
    grep -q '^State:.*Z' "/proc/$gui/status" && break
    sleep 1
done
if [ -d "/proc/$gui" ] && ! grep -q '^State:.*Z' "/proc/$gui/status"; then
    kill -CONT "$parent"
    restored=1
    echo 'Stock GUI did not terminate; test cancelled' >&2
    exit 1
fi
[ "$(pidof ar_ldy_gnd)" = "$backend" ] && [ "$(pidof daemon_sdio)" = "$radio" ]
"$directory/kestrel-ui" --stock --fb /dev/fb0 --assets "$directory" --seconds "$seconds" \
    --show-menu --screenshot "$directory/overlay.pam" > "$directory/ui.log" 2>&1 &
ui_pid=$!
echo "KESTREL_UI_PID=$ui_pid DURATION=$seconds"
if ! wait "$ui_pid"; then
    ui_pid=0
    cat "$directory/ui.log"
    exit 1
fi
ui_pid=0
cat "$directory/ui.log"
[ "$(pidof ar_ldy_gnd)" = "$backend" ] && [ "$(pidof daemon_sdio)" = "$radio" ]
cleanup
[ -n "$(pidof GlassesUI)" ]
