#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-dso-roundtrip.XXXXXX")
display=:94
xvfb_pid=
cleanup() {
    [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT

mkdir -p "$root/base/scripts"
printf '%s\n' 'function DsoProbe(%value) { return %value + 1; }' >"$root/base/scripts/probe.cs"
printf '%s\n' 'function DsoArithmetic(%a,%b) { return %a * %b + 2; }' >"$root/base/scripts/arithmetic.cs"
printf '%s\n' 'function DsoBranch(%x) { if (%x) return "yes"; return "no"; }' >"$root/base/scripts/branch.cs"
printf '%s\n' 'function DsoLoop(%n) { %sum = 0; for (%i = 0; %i < %n; %i++) { %sum += %i; } return %sum; }' >"$root/base/scripts/loop.cs"
printf '%s\n' 'function DsoJoin(%a,%b) { return %a @ "-" @ %b; }' >"$root/base/scripts/join.cs"
printf '%s\n' 'package DsoPkg { function DsoPack(%x) { return %x + 3; } }' >"$root/base/scripts/package.cs"
printf '%s\n' 'function DsoNS::Scale(%x) { return %x * 3; }' >"$root/base/scripts/namespace.cs"
printf '%s\n' 'function DsoWords() { return getWords("a b c d", 1, 2); } function DsoFields() { return getFields("a\tb\tc\td", 1, 2); } function DsoWord() { return getWord("a b c", 1) @ ":" @ getWordCount("a b c") @ ":" @ firstWord("a b c"); }' >"$root/base/scripts/wordrange.cs"
printf '%s\n' 'function DsoStrings() { return setWord("a b", 1, "c") @ ":" @ restWords("a b c") @ ":" @ getFieldCount("a\tb\tc") @ ":" @ strReplace("a-b", "-", "_"); }' >"$root/base/scripts/strings.cs"
printf '%s\n' 'function DsoVoid(%value) { DsoProbe(%value); return; }' >"$root/base/scripts/void.cs"
printf '%s\n' 'package HiddenPkg { function HiddenValue() { return 7; } } function DsoCallsHidden() { return HiddenValue(); }' >"$root/base/scripts/hidden.cs"
printf '%s\n' 'function DsoMakeObject() { new SimGroup(DsoGroup) { marker = "ok"; }; return DsoGroup; } function DsoReadUpper() { return DsoGroup.MARKER; }' >"$root/base/scripts/object.cs"
printf '%s\n' '$compilerScript = "/home/methodown/torch/torque-dso.js"; compile("scripts/probe.cs"); exec("scripts/probe.cs"); echo(DsoProbe(4)); compile("scripts/arithmetic.cs"); exec("scripts/arithmetic.cs"); echo(DsoArithmetic(3,4)); compile("scripts/branch.cs"); exec("scripts/branch.cs"); echo(DsoBranch(1)); compile("scripts/loop.cs"); exec("scripts/loop.cs"); echo(DsoLoop(5)); compile("scripts/join.cs"); exec("scripts/join.cs"); echo(DsoJoin("a","b")); compile("scripts/package.cs"); exec("scripts/package.cs"); activatePackage("DsoPkg"); echo(DsoPack(4)); compile("scripts/namespace.cs"); exec("scripts/namespace.cs"); echo(DsoNS::Scale(2)); compile("scripts/wordrange.cs"); exec("scripts/wordrange.cs"); echo(DsoWords()); echo(DsoFields()); echo(DsoWord()); compile("scripts/strings.cs"); exec("scripts/strings.cs"); echo(DsoStrings()); compile("scripts/void.cs"); exec("scripts/void.cs"); echo(DsoVoid(9) @ "x"); compile("scripts/hidden.cs"); exec("scripts/hidden.cs"); echo(DsoCallsHidden()); activatePackage("HiddenPkg"); echo(DsoCallsHidden()); deactivatePackage("HiddenPkg"); echo(DsoCallsHidden()); compile("scripts/object.cs"); exec("scripts/object.cs"); echo(DsoMakeObject()); echo(DsoReadUpper()); echo(getField("DsoGroup", "marker")); quit();' >"$root/base/scripts/driver.cs"

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

grep -q "VM: loaded 'scripts/probe.cs.dso' v174" "$root/client.log"
grep -qE '\[INFO\] 5$' "$root/client.log"
grep -qE '\[INFO\] 14$' "$root/client.log"
grep -qE '\[INFO\] yes$' "$root/client.log"
grep -qE '\[INFO\] 10$' "$root/client.log"
grep -qE '\[INFO\] a-b$' "$root/client.log"
grep -qE '\[INFO\] 7$' "$root/client.log"
grep -qF '[INFO] DsoGroup' "$root/client.log"
grep -qF '[INFO] ok' "$root/client.log"
grep -qF '[INFO] ok' "$root/client.log"
grep -qE '\[INFO\] 6$' "$root/client.log"
grep -qE '\[INFO\] b c$' "$root/client.log"
grep -qF $'[INFO] b\tc' "$root/client.log"
grep -qE '\[INFO\] b:3:a$' "$root/client.log"
grep -qF $'[INFO] a c:b c:3:a_b' "$root/client.log"
grep -qF '[INFO] x' "$root/client.log"
grep -qE '\[INFO\] 0$' "$root/client.log"
grep -qE '\[INFO\] 7$' "$root/client.log"
test -s "$root/out/base/scripts/probe.cs.dso"
test -s "$root/out/base/scripts/probe.cs.dso.deps"
printf 'script DSO round trip passed\n'
