#!/usr/bin/env bash
set -euo pipefail

torch=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/torch-object-semantics.XXXXXX")
display=:97
xvfb_pid=
cleanup() { [[ -n "$xvfb_pid" ]] && kill "$xvfb_pid" 2>/dev/null || true; rm -rf "$root"; }
trap cleanup EXIT

mkdir -p "$root/base/scripts"
cat >"$root/base/scripts/objects.cs" <<'CS'
// hud.cs forwards addLine to $Hud[%tag].addLine; with no such method the
// call must not re-enter the global function.
function addLine(%tag) { return $Hud[%tag].addLine(%tag); }
function run() {
   $Hud[1] = "";
   $Hud[1].data[3, 0] = "q";
   echo("R1 [" @ $Hud[1].data[3, 0] @ "]");
   echo("R2 " @ isObject(""));
   %line = 3;
   if ($Hud[1].data[%line, 0] !$= "")
      for (%i = 0; %i < $Hud[1].numCol; %i++)
      {
         echo("never");
      }
   echo("R3 skipped");
   echo("R4 [" @ addLine(1) @ "]");
}
run();
// SimObject ids: new returns the id; anonymous objects are distinct; the
// class field is a dispatch namespace, onAdd included.
function IdSpace::onAdd(%this) { $idOnAdd = %this; }
function IdSpace::who(%this) { return "who" @ %this.getId(); }
function ids() {
   %named = new ScriptObject(IdNamed) { v = 1; };
   %a = new ScriptObject() { v = 2; };
   %b = new ScriptObject() { v = 3; class = IdSpace; };
   echo("R7 " @ (%named == IdNamed.getId()) @ " " @ (%named == nameToId(IdNamed)) @ " " @ (%named >= 1027));
   echo("R8 " @ (%a != %b) @ " " @ %a.v @ %b.v @ " [" @ %a.getName() @ "] " @ IdNamed.getName());
   echo("R9 " @ ($idOnAdd == %b) @ " " @ (%b.who() $= "who" @ %b));
   %g = new SimGroup();
   %g.add(%a);
   echo("R10 " @ %g.getCount() @ " " @ (%g.getObject(0) == %a));
   %a.delete();
   echo("R11 " @ isObject(%a) @ isObject(%b) @ " " @ ("1" + 1));
}
ids();
// A body started by the scheduler (native code) survives a nested eval.
function later() {
   eval("echo(\"R5 inner\");");
   echo("R6 after eval");
   quit();
}
schedule(10, 0, later);
CS
Xvfb "$display" -screen 0 1024x768x24 -ac >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do [[ -e "/tmp/.X11-unix/X${display#:}" ]] && break; sleep 0.1; done
kill -0 "$xvfb_pid" 2>/dev/null
timeout --kill-after=3s 15s env DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 "$torch" -data "$root" -mod base \
    -output "$root/out" -demo-mode -nologin -exec scripts/objects.cs -quit-after-frames 600 \
    >"$root/client.log" 2>&1
grep -qF '[INFO] R1 []' "$root/client.log"
grep -qF '[INFO] R2 0' "$root/client.log"
grep -qF '[INFO] R3 skipped' "$root/client.log"
grep -qE "\[INFO\] R4 \[0?\]" "$root/client.log"
grep -qF '[INFO] R5 inner' "$root/client.log"
grep -qF '[INFO] R6 after eval' "$root/client.log"
grep -qF '[INFO] R7 1 1 1' "$root/client.log"
grep -qF '[INFO] R8 1 23 [] IdNamed' "$root/client.log"
grep -qF '[INFO] R9 1 1' "$root/client.log"
grep -qF '[INFO] R10 1 1' "$root/client.log"
grep -qF '[INFO] R11 01 2' "$root/client.log"
! grep -qF 'Expected token' "$root/client.log"
! grep -qF 'never' "$root/client.log"
! grep -qF 'call depth limit' "$root/client.log"
! grep -qF 'Unexpected token' "$root/client.log"
printf 'script object semantics passed\n'
