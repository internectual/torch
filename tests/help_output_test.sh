#!/usr/bin/env bash
set -eu

client=$1
server=$2

client_help=$($client --help)
server_help=$($server --help)

for option in "-demo <file.rec>" "--demo <file.rec>" "-playdemo <file.rec>" \
    "-demo-mode" "-demo-master-server <url>" "-mapper <map>" "-output <dir>" "-mod <path>" "-exec,-e <file>" \
    "demoMasterServer" "demoAllowConnect" "demoAllowWatch" "default: empty = LAN" \
    "isDemo()" "isDemoPlaying()" \
    "-quit-after-frames <n>" "connect <host> [port]" \
    "watchServer <host:port>" "loadMission <name>" "startServer [port] [mission]" \
    "TORCH-RUN-START"; do
    case "$client_help" in
        *"$option"*) ;;
        *) printf 'client help missing: %s\n' "$option" >&2; exit 1 ;;
    esac
done

for option in "-p <port>" "-m <mission>" "-data <dir>" "-output <dir>" "console.log"; do
    case "$server_help" in
        *"$option"*) ;;
        *) printf 'server help missing: %s\n' "$option" >&2; exit 1 ;;
    esac
done
