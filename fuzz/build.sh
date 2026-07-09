#!/usr/bin/env bash
# Build the libFuzzer harness headlessly (no GL / engine needed).
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=src
FUZZ=fuzz

clang++ -std=c++20 -O1 -g \
  -fsanitize=fuzzer,address,undefined \
  -DGLEW_STATIC -I"$SRC" \
  "$FUZZ/fuzz_loaders.cpp" \
  "$FUZZ/console_stub.cpp" \
  "$FUZZ/engine_stub.cpp" \
  "$FUZZ/render_stubs.cpp" \
  "$SRC/render/dif_loader.cpp" \
  "$SRC/render/dts_loader.cpp" \
  "$SRC/render/glb_loader.cpp" \
  "$SRC/script/dso_reader.cpp" \
  "$SRC/fs/vol_archive.cpp" \
  "$SRC/fs/vl2_archive.cpp" \
  "$SRC/fs/file_system.cpp" \
  "$SRC/core/math.cpp" \
  -o "$FUZZ/fuzz_loaders" -lpthread -lz

echo "BUILD OK -> $FUZZ/fuzz_loaders"

echo "=== building fuzz_net (network protocol decoders) ==="
clang++ -std=c++20 -O1 -g -fsanitize=fuzzer,address,undefined \
  -DGLEW_STATIC -I "$SRC" \
  "$FUZZ/fuzz_net.cpp" \
  "$FUZZ/console_stub.cpp" \
  -o "$FUZZ/fuzz_net" -lpthread
echo "BUILD OK -> $FUZZ/fuzz_net"
