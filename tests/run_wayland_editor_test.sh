#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# End-to-end check of the VST3 3.8.0 Wayland plugin editor.
#
#   run_wayland_editor_test.sh <wayland_editor_host> <plugin.vst3>
#
#   Xvfb :N  ->  gnome-shell --nested (real mutter)  ->  wayland_editor_host
#
# A nested compositor, never the user's session: this maps windows and, before
# the roundtrip workaround in ~Connection, could take a whole GNOME 46 desktop
# down with it. Same harness SynthEditWayland/autotest.sh uses, and the same
# reasoning - a private display, a private socket, and a trap that kills only
# what it started.
#
# Verified by pixels, because the plugin's buffers go to the compositor and
# never pass through the host's address space: the host paints its window a
# colour no plugin would draw, and the plugin's subsurface has to cover part of
# it. Set WORKDIR to keep the capture and the compositor log.
# ---------------------------------------------------------------------------
set -u

HOSTBIN=${1:?usage: $0 <wayland_editor_host> <plugin.vst3>}
PLUGIN=${2:?usage: $0 <wayland_editor_host> <plugin.vst3>}

TOOLS=${WLTEST_TOOLS:-$HOME/.cache/wayland-testtools}
OUT=${WORKDIR:-$(mktemp -d)}
mkdir -p "$OUT"

[ -x "$TOOLS/usr/bin/Xvfb" ] || { echo "SKIP: no Xvfb in $TOOLS"; exit 77; }
command -v gnome-shell >/dev/null || { echo "SKIP: no gnome-shell for a nested compositor"; exit 77; }

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:$TOOLS/usr/lib/x86_64-linux-gnu

DISP=""
for n in $(seq 40 60); do
    [ -e "/tmp/.X11-unix/X$n" ] || { DISP=":$n"; break; }
done
[ -n "$DISP" ] || { echo "FAIL: no free X display"; exit 1; }
SOCK=gmpi-wl-test-$$

XVFB=""; NEST_PGID=""
cleanup() {
    # Process GROUP: a nested shell spawns children, and killing only the
    # shell's own pid leaves them running. Hundreds accumulated that way once.
    [ -n "$NEST_PGID" ] && kill -- -"$NEST_PGID" 2>/dev/null
    [ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null
}
trap cleanup EXIT

"$TOOLS/usr/bin/Xvfb" "$DISP" -screen 0 1400x900x24 >/dev/null 2>&1 & XVFB=$!
for _ in $(seq 1 40); do
    DISPLAY=$DISP "$TOOLS/usr/bin/xdotool" getdisplaygeometry >/dev/null 2>&1 && break
    sleep 0.25
done

setsid env -u WAYLAND_DISPLAY DISPLAY="$DISP" GNOME_SHELL_DISABLE_EXTENSIONS=1 dbus-run-session -- \
    gnome-shell --nested --wayland --wayland-display="$SOCK" >"$OUT/compositor.log" 2>&1 & NEST=$!
NEST_PGID=$(ps -o pgid= -p "$NEST" 2>/dev/null | tr -d " ")

for _ in $(seq 1 80); do [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && break; sleep 0.5; done
[ -S "$XDG_RUNTIME_DIR/$SOCK" ] || { echo "FAIL: nested compositor never came up"; exit 1; }
sleep 3
DISPLAY=$DISP "$TOOLS/usr/bin/xdotool" key Escape   # leave the Overview if it started there
sleep 1

env WAYLAND_DISPLAY="$SOCK" "$HOSTBIN" "$PLUGIN" 4000 >"$OUT/host.log" 2>&1 & HOST_PID=$!

# BEFORE: the host holds its window mapped and plugin-free for two seconds
# exactly so this can be taken. Wait for the window rather than guessing.
for _ in $(seq 1 40); do grep -q "host window mapped" "$OUT/host.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.8
xwd -display "$DISP" -root -silent > "$OUT/before.xwd" 2>/dev/null

# AFTER: past the settle, past attach, with the editor running. Both captures
# happen while the host is up; its window goes away when it exits.
sleep 4
xwd -display "$DISP" -root -silent > "$OUT/after.xwd" 2>/dev/null

wait $HOST_PID
RC=$?
cat "$OUT/host.log"

[ $RC -eq 77 ] && { echo "SKIP"; exit 77; }
[ $RC -eq 0 ] || { echo "FAIL: host exited $RC"; exit 1; }

python3 "$(dirname "$0")/check_wayland_capture.py" "$OUT/before.xwd" "$OUT/after.xwd" || exit 1

echo "PASS: Wayland editor attached, ran the host's run loop, and drew into a subsurface"
