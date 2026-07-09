#!/bin/bash
# Benchmark script: start server, spawn bots, measure performance
set -e

SERVER_BIN="${1:-./build/torch_server}"
BOT_COUNT="${2:-50}"
DURATION="${3:-30}"

echo "=== Torch Benchmark ==="
echo "Server: $SERVER_BIN"
echo "Bots: $BOT_COUNT"
echo "Duration: ${DURATION}s"
echo ""

# Start server in background
$SERVER_BIN -p 28000 -m test &
SERVER_PID=$!
sleep 2

# Spawn bots via console
for i in $(seq 1 $BOT_COUNT); do
    echo "sv_addbot" | socat - TCP:localhost:28000 2>/dev/null || true
done

echo "Spawned $BOT_COUNT bots, measuring for ${DURATION}s..."
sleep $DURATION

# Get stats
kill -USR1 $SERVER_PID 2>/dev/null || true
sleep 1

# Stop server
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Benchmark Complete ==="
