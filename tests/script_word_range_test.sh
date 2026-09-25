#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-word-range.XXXXXX")
display=:91
xvfb_pid=
cleanup() { [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true; rm -rf "$root"; }
trap cleanup EXIT

mkdir -p "$root/base/scripts"
printf '$calls = 0; function bump(%%value) { $calls++; return %%value; } $result = 0 ? bump(1) : bump(2); $nullResult = null ? 1 : 0; echo(getWords("a b c d e f", 2, 4)); echo(getFields("a\\tb\\tc\\td\\te", 1, 3)); echo(strReplace("abc", "", "x")); echo($result); echo($calls); echo($nullResult); quit();\n' >"$root/base/scripts/range.cs"
Xvfb "$display" -screen 0 1024x768x24 -ac >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do [[ -e "/tmp/.X11-unix/X${display#:}" ]] && break; sleep 0.1; done
kill -0 "$xvfb_pid" 2>/dev/null
timeout --kill-after=3s 15s env DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 "$torch" -data "$root" -mod base \
    -output "$root/out" -demo-mode -nologin -exec scripts/range.cs -quit-after-frames 2 \
    >"$root/client.log" 2>&1
grep -qF '[INFO] c d e' "$root/client.log"
grep -qF $'[INFO] b\tc\td' "$root/client.log"
grep -qF '[INFO] abc' "$root/client.log"
grep -qF '[INFO] 2' "$root/client.log"
grep -qF '[INFO] 1' "$root/client.log"
grep -qF '[INFO] 0' "$root/client.log"
printf 'script word range passed\n'
