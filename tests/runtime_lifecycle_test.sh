#!/usr/bin/env bash
set -euo pipefail

torch=$1
server=$2
data=${TORCH_TEST_DATA:-"$HOME/.torch"}
display=:97
log_dir=$(mktemp -d "${TMPDIR:-/tmp}/torch-lifecycle.XXXXXX")
xvfb_pid=

cleanup() {
    if [[ -n "$xvfb_pid" ]]; then kill "$xvfb_pid" 2>/dev/null || true; fi
    rm -rf "$log_dir"
}
trap cleanup EXIT
source_dir=$(cd "$(dirname "$0")/.." && pwd)

Xvfb "$display" -screen 0 1024x768x24 -ac >"$log_dir/xvfb.log" 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do
    if kill -0 "$xvfb_pid" 2>/dev/null && [[ -e "/tmp/.X11-unix/X${display#:}" ]]; then break; fi
    sleep 0.1
done
if ! kill -0 "$xvfb_pid" 2>/dev/null; then
    printf 'Xvfb failed to start\n' >&2
    exit 1
fi

run_client() {
    local name=$1
    shift
    if ! timeout --kill-after=3s 20s env \
        DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
        "$torch" -data "$data" -output "$log_dir/output" "$@" \
         >"$log_dir/$name.out" 2>&1; then
        printf '%s failed; log: %s\n' "$name" "$log_dir/$name.out" >&2
         return 1
    fi
    if ! grep -q "Torch v" "$log_dir/output/console.log"; then
        printf '%s did not produce a complete console.log\n' "$name" >&2
        return 1
    fi
}

run_client_expect_failure() {
    local name=$1
    shift
    if timeout --kill-after=3s 10s env \
        DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
        "$torch" "$@" >"$log_dir/$name.out" 2>&1; then
        printf '%s unexpectedly succeeded\n' "$name" >&2
        return 1
    fi
}

for iteration in 1 2 3; do
    # Let the always-rendered dev-panel object tree visit the full retail
    # script object registry; sparse startup frames miss null registry slots.
    run_client "client-$iteration" -nologin -quit-after-frames 8
done

run_client_expect_failure invalid-frames -quit-after-frames not-a-number
run_client_expect_failure missing-mapper -mapper

demo="$data/base/recordings/treachery.rec"
if [[ ! -f "$demo" ]]; then
    printf 'missing demo recording: %s\n' "$demo" >&2
    exit 1
fi
run_client demo -demo recordings/treachery.rec -quit-after-frames 3
run_client playdemo -playdemo recordings/treachery.rec -quit-after-frames 3
run_client demo-mode -demo-mode -quit-after-frames 3
for demo_name in demo playdemo; do
    if ! grep -q "Loading demo:" "$log_dir/$demo_name.out" ||
       ! grep -q "TORCH-RUN-START" "$log_dir/$demo_name.out"; then
        printf '%s did not reach demo startup\n' "$demo_name" >&2
        exit 1
    fi
done
if ! grep -q "TORCH-RUN-START" "$log_dir/demo-mode.out"; then
    printf '%s did not reach demo-build startup\n' demo-mode
    exit 1
fi

run_client mapper -mapper missions/Katabatic.mis -quit-after-frames 2
if ! grep -q "Mapper mode:" "$log_dir/mapper.out"; then
    printf 'mapper mode did not start\n' >&2
    exit 1
fi

commands="$log_dir/runtime_commands.cs"
printf 'connect("invalid.invalid", 28000); watchServer("invalid.invalid:28000"); loadMission("missions/does-not-exist.mis"); quit();\n' >"$commands"
run_client command-failures -exec "$commands" -quit-after-frames 2
if ! grep -q "Cannot resolve\|Connection failed\|Invalid mission" "$log_dir/command-failures.out"; then
    printf 'command failure paths did not report a failure\n' >&2
    exit 1
fi

# The dedicated server is the retail DedicatedServer launch: run from the
# source tree it reads torch.cfg (the Tribes 2 dataDir and init script),
# hosts the mission, opens its UDP port and takes TorqueScript on stdin.
# Without an install it stops at the missing init script.
install=$(sed -n 's/^dataDir = //p' "$source_dir/torch.cfg" 2>/dev/null)
install=${install/#\~/$HOME}
if ! (cd "$source_dir" && (sleep 8; printf 'quit();\n') | timeout --kill-after=3s 30s \
    "$server" -nologin -output "$log_dir/server-output" \
    -mission TWL_Minotaur CTF) >"$log_dir/server.out" 2>&1; then
    printf 'dedicated server smoke failed\n' >&2
    exit 1
fi
if [[ -n "$install" && -f "$install/console_start.cs" ]]; then
    grep -q "UDP initialized on port" "$log_dir/server.out" || {
        printf 'dedicated server did not open its port\n' >&2; exit 1; }
else
    grep -q "Init script not found" "$log_dir/server.out" || {
        printf 'dedicated server without an install did not report it\n' >&2; exit 1; }
fi

printf 'runtime lifecycle smoke passed\n'
