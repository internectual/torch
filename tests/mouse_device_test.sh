#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-mouse-device.XXXXXX")
display=:92
xvfb_pid=
cleanup() {
    [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT

mkdir -p "$root/base/scripts"
printf 'echo(disableMouse()); echo(enableMouse()); quit();\n' >"$root/base/scripts/driver.cs"
Xvfb "$display" -screen 0 1024x768x24 -ac >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do
    [[ -e "/tmp/.X11-unix/X${display#:}" ]] && break
    sleep 0.1
done
kill -0 "$xvfb_pid" 2>/dev/null

timeout --kill-after=3s 15s env \
    DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
    "$torch" -data "$root" -mod base -output "$root/out" -demo-mode \
    -nologin -exec scripts/driver.cs -quit-after-frames 2 >"$root/client.log" 2>&1

test "$(grep -cE '\[INFO\] 1$' "$root/client.log")" -ge 2
printf 'mouse device controls passed\n'
