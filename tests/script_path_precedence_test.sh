#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-script-path.XXXXXX")
display=:96
xvfb_pid=
cleanup() {
    [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT

mkdir -p "$root/base/scripts" "$root/classic/scripts"
printf 'echo("BASE-CHILD");\nquit();\n' >"$root/base/scripts/Child.CS"
printf 'echo("CLASSIC-CHILD");\nquit();\n' >"$root/classic/scripts/Child.CS"
printf 'exec("scripts/child.cs");\n' >"$root/base/scripts/Parent.CS"
printf 'exec("scripts/child.cs");\n' >"$root/classic/scripts/Parent.CS"

Xvfb "$display" -screen 0 1024x768x24 -ac >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do
    [[ -e "/tmp/.X11-unix/X${display#:}" ]] && break
    sleep 0.1
done
if ! kill -0 "$xvfb_pid" 2>/dev/null; then exit 1; fi

run_case() {
    local mode=$1
    local output="$root/$mode-output"
    timeout --kill-after=3s 15s env DISPLAY="$display" SDL_VIDEODRIVER=x11 \
        SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
        "$torch" -data "$root" -mod "$mode" -output "$output" -demo-mode \
        -nologin -exec scripts/parent.cs -quit-after-frames 2 >"$root/$mode.log" 2>&1
    grep -q "${mode^^}-CHILD" "$root/$mode.log"
}

run_case classic
run_case base
printf 'script path precedence passed\n'
