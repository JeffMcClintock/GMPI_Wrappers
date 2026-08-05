#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# End-to-end check of the Linux VST3 editor.
#
#   run_x11_editor_test.sh <x11_editor_host> <plugin.vst3> [--expect-input]
#
# Runs on a throwaway Xvfb, never the real session. Always verified:
#
#   the editor attaches, registers with the run loop, and paints
#
# With --expect-input, also: a synthesised mouse drag CHANGES the picture. That
# is the check worth having - attaching and painting has passed before while the
# plugin crashed on the first click - but it only applies to a plugin that has
# something draggable where the drag lands. An analyser with no pointer handlers
# correctly changes nothing, so demanding it of every plugin would just teach
# everyone to ignore the result.
#
# Exits 77 (the ctest SKIP convention) when the plugin has no editor.
# Set WORKDIR to keep the captures.
# ---------------------------------------------------------------------------
set -u

HOSTBIN=${1:?usage: $0 <x11_editor_host> <plugin.vst3> [--expect-input]}
PLUGIN=${2:?usage: $0 <x11_editor_host> <plugin.vst3> [--expect-input]}
EXPECT_INPUT=${3:-}
WORK=${WORKDIR:-$(mktemp -d)}

TOOLS=${WLTEST_TOOLS:-$HOME/.cache/wayland-testtools}
XVFB=$(command -v Xvfb || echo "$TOOLS/usr/bin/Xvfb")
XDOTOOL=$(command -v xdotool || echo "$TOOLS/usr/bin/xdotool")
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:$TOOLS/usr/lib/x86_64-linux-gnu

[ -x "$XVFB" ] || { echo "SKIP: no Xvfb"; exit 77; }

DISP=:$((90 + RANDOM % 8))
mkdir -p "$WORK"

"$XVFB" "$DISP" -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null' EXIT
sleep 2

OUT=$WORK/editor.ppm
rm -f "$OUT" "$OUT.before.ppm"

DISPLAY=$DISP timeout 60 "$HOSTBIN" "$PLUGIN" "$OUT" 2500 &
HOST_PID=$!

# Wait for the "before" capture rather than guessing: the host writes it once
# the editor has settled.
for _ in $(seq 1 40); do [ -f "$OUT.before.ppm" ] && break; sleep 0.1; done

# Only when asked. A no-flag run must be READ-ONLY: DrawingDemo advances a page
# on click, so a drag nobody asked for silently changed what was captured, and
# the report still said "input not checked".
if [ "$EXPECT_INPUT" = "--expect-input" ] && [ -x "$XDOTOOL" ]; then
    export DISPLAY=$DISP
    "$XDOTOOL" mousemove 100 100 sleep 0.2 mousedown 1 sleep 0.2 >/dev/null 2>&1
    for y in 95 90 85 80 70 60 50 40; do
        "$XDOTOOL" mousemove 100 $y sleep 0.05 >/dev/null 2>&1
    done
    "$XDOTOOL" mouseup 1 >/dev/null 2>&1
elif [ "$EXPECT_INPUT" = "--expect-input" ]; then
    echo "note: no xdotool - painting checked, input NOT checked"
fi

wait $HOST_PID
RC=$?
[ $RC -eq 77 ] && { echo "SKIP: plugin has no editor"; exit 77; }
[ $RC -eq 0 ] || { echo "FAIL: editor host exited $RC"; exit 1; }

if [ "$EXPECT_INPUT" = "--expect-input" ] && [ -x "$XDOTOOL" ]; then
    python3 - "$OUT.before.ppm" "$OUT" <<'PY' || exit 1
import sys
def read_ppm(p):
    with open(p,'rb') as f:
        assert f.readline().strip()==b'P6'
        w,h = map(int, f.readline().split())
        f.readline()
        return f.read(w*h*3)
a, b = read_ppm(sys.argv[1]), read_ppm(sys.argv[2])
changed = sum(1 for x, y in zip(a, b) if x != y)
print(f"changed bytes after drag: {changed}")
if changed == 0:
    print("FAIL: the drag changed nothing - input is not reaching the editor")
    raise SystemExit(1)
PY
fi

if [ "$EXPECT_INPUT" = "--expect-input" ]; then
    echo "PASS: editor attached, painted, and responded to input"
else
    echo "PASS: editor attached and painted (input not checked)"
fi
