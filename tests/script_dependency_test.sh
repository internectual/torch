#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-script-deps.XXXXXX")
display=:97
xvfb_pid=
cleanup() { [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true; rm -rf "$root"; }
trap cleanup EXIT

mkdir -p "$root/base/scripts"
printf 'echo("CHILD-ONE");\n' >"$root/base/scripts/child.cs"
printf 'exec("scripts/child.cs"); echo("PARENT");\n' >"$root/base/scripts/parent.cs"
printf 'exec("scripts/cycle-b.cs"); echo("CYCLE-A");\n' >"$root/base/scripts/cycle-a.cs"
printf 'exec("scripts/cycle-a.cs"); echo("CYCLE-B");\n' >"$root/base/scripts/cycle-b.cs"
printf 'function PackageProbe() { return "base"; } package Low { function PackageProbe() { return Parent::PackageProbe() @ "-low"; } } package High { function PackageProbe() { return Parent::PackageProbe() @ "-high"; } } activatePackage("Low"); activatePackage("High"); echo(PackageProbe());\n' >"$root/base/scripts/packages.cs"
printf 'function ReloadProbe() { return "reload"; } echo(ReloadProbe());\n' >"$root/base/scripts/reload.cs"
printf 'exec("scripts/reload.cs"); exec("scripts/reload.cs");\n' >"$root/base/scripts/reload-driver.cs"

Xvfb "$display" -screen 0 1024x768x24 -ac >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do [[ -e "/tmp/.X11-unix/X${display#:}" ]] && break; sleep 0.1; done
kill -0 "$xvfb_pid" 2>/dev/null || exit 1

run() {
    local name=$1 script=$2
    timeout --kill-after=3s 15s env DISPLAY="$display" SDL_VIDEODRIVER=x11 \
        SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
        "$torch" -data "$root" -mod base -output "$root/out" -demo-mode \
        -nologin -exec "$script" -quit-after-frames 2 >"$root/$name.log" 2>&1
}

run first scripts/parent.cs
grep -q 'PARENT' "$root/first.log"
sleep 2
printf 'echo("CHILD-TWO");\n' >"$root/base/scripts/child.cs"
run second scripts/parent.cs
grep -q 'Exec: scripts/parent.cs' "$root/second.log"
run circular scripts/cycle-a.cs
grep -q 'CYCLE-A' "$root/circular.log"
run packages scripts/packages.cs
grep -q 'base-low-high' "$root/packages.log"
run reload scripts/reload-driver.cs
test "$(grep -c '\] reload$' "$root/reload.log")" -eq 2
printf 'script dependency tracking passed\n'
