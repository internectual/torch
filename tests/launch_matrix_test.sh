#!/usr/bin/env bash
set -euo pipefail

torch=$1
data=${TORCH_TEST_DATA:-"$HOME/.torch"}
display=:98
log_dir=$(mktemp -d "${TMPDIR:-/tmp}/torch-matrix.XXXXXX")
xvfb_pid=

cleanup() {
    if [[ -n "$xvfb_pid" ]]; then kill "$xvfb_pid" 2>/dev/null || true; fi
    rm -rf "$log_dir"
}
trap cleanup EXIT

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

run_case() {
    local name=$1
    shift
    timeout --kill-after=3s 20s env \
        DISPLAY="$display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        ALSOFT_DRIVERS=null LIBGL_ALWAYS_SOFTWARE=1 \
        "$torch" -data "$data" -output "$log_dir/$name-output" \
        "$@" -quit-after-frames 2 >"$log_dir/$name.log" 2>&1
}

demo="$data/base/recordings/treachery.rec"
if [[ ! -f "$demo" ]]; then
    printf 'missing demo recording: %s\n' "$demo" >&2
    exit 1
fi

run_case retail -debug -nologin || exit 1
run_case online -online -nologin || exit 1
run_case playback -playdemo recordings/treachery.rec || exit 1
run_case demo-mode -demo-mode -nologin || exit 1
mission=missions/Katabatic.mis
run_case mapper -mapper "$mission" -online || exit 1
run_case preview -preview "$mission" -online || exit 1
run_case data-mod -data "$data" -mod base -nologin || exit 1
run_case demo-policy -demo-mode -demo-master-server https://demo.invalid/list -online -nologin || exit 1

for name in retail online playback demo-mode data-mod demo-policy; do
    log="$log_dir/$name.log"
    grep -q 'TORCH-RUN-START' "$log" || { printf '%s did not enter run loop\n' "$name" >&2; exit 1; }
    grep -q 'Goodbye' "$log" || { printf '%s did not cleanly shut down\n' "$name" >&2; exit 1; }
done
for name in mapper preview; do
    grep -q 'Goodbye' "$log_dir/$name.log" || { printf '%s did not clean up\n' "$name" >&2; exit 1; }
done
grep -q 'Bootstrap:' "$log_dir/retail.log" || exit 1
for name in playback demo-mode mapper; do
    grep -q -- '-nologin: dev panel\|Mapper mode:' "$log_dir/$name.log" || exit 1
done
grep -q 'Loading demo:' "$log_dir/playback.log" || exit 1
if [[ -f "$data/base/$mission" ]]; then
    grep -q 'Mapper mode: free-fly camera active' "$log_dir/mapper.log" || exit 1
    grep -q 'Preview:' "$log_dir/preview.log" || exit 1
else
    grep -q "Mapper mode: failed to load\|Mission unavailable" "$log_dir/mapper.log" || exit 1
    grep -q "Mission unavailable" "$log_dir/preview.log" || exit 1
fi

printf 'launch matrix runtime smoke passed\n'
