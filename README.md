# Torch - Tribes 2 Engine Reimplementation

A cross-platform reimplementation of the Tribes 2 / Torque Game Engine
networking and gameplay core.

Runtime assets come exclusively from an untouched Tribes 2 installation. Torch
does not use GLB conversions, generated replacement assets, or substitute
models and textures. Unsupported native assets fail explicitly so rendering
bugs can be fixed against the original DTS, DIF, TER, GUI, and script data.

## Building

### Dependencies
- CMake 3.20+
- C++20 compiler (GCC 11+, Clang 14+)
- SDL3, GLEW, OpenAL, GLU, zlib, libvorbis

### Linux
```sh
sudo apt install cmake g++ libsdl3-dev libglew-dev libopenal-dev \
                 libglu1-mesa-dev libvorbis-dev zlib1g-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (cross-compile from Linux)
```sh
sudo apt install mingw-w64 cmake

cmake -B build-win \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j$(nproc)
```

### macOS
```sh
brew install cmake sdl3 glew openal-soft glm zlib libvorbis
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Running

### Client
```sh
./build/torch
```

### Dedicated Server
```sh
./build/torch_server -nologin -mission TWL_Minotaur CTF
```
`torch_server` is the retail DedicatedServer launch (`console_start.cs`
`-dedicated`) without a window: the stock server scripts host the mission,
the port is `$Host::Port` (from `-serverprefs <file>`), and TorqueScript is
read from stdin (`quit();` stops it). Clients join with the stock
`JoinGame("host:port")` / `connect()`.

Use `-data <dir>` to select the untouched Tribes 2 installation, `-mod <path>`
to select its active mod, and `-output <dir>` to select writable runtime state
and logs. The default output directory is `~/.torch`. Precedence is built-in
defaults, `torch.cfg`, then command-line options; later console/script writes
are runtime-only overrides.

Dynamic projectile and explosion lights use the forward renderer's bounded
eight-light point collection and are applied to terrain and shape/interior
materials alongside existing sun, shadow, fog, normal-map, and lightmap terms.
They currently do not cast point-light shadows or perform interior portal/BSP
occlusion; the safe fallback is finite-radius attenuation with no dynamic
light contribution when the collection is unavailable or full.

Options:
- `-p <port>`  – Server port (default: 28000)
- `-m <name>`  – Mission to load (e.g. `test`, `deathmatch`)
- `-data <dir>` – Tribes 2 data directory
- `-output <dir>` – Runtime output directory; `console.log` is written here
- `-h`         – Help

The client accepts `-demo`, `--demo`, and `-playdemo` as equivalent demo launch
aliases for recording playback. `-demo-mode` is a separate explicit demo-build
mode; it does not start playback. `-mapper <map>` loads a mission without gameplay for free-fly
inspection; `-mapper-camera <n>` selects an authored observer camera.
`-exec <file>` (or `-e`) executes a TorqueScript file after GUI startup.
`-quit-after-frames <n>` provides a bounded run for automation; `0` means no
frame limit. `-debug` enables debug logging. The client writes `console.log`
under the output directory and emits `TORCH-RUN-START` on stderr when the main
loop begins.

## Console Commands

### Server
Server administration is the stock scripts' (`server.cs`: `kick`, `ban`;
`admin.cs`: votes) on the dedicated server's console.

Environment TorqueScript natives:

| Native | Behavior |
|--------|----------|
| `setWaterLevel([nameOrIndex,] level)` | Move the first or selected authored water surface |
| `setWaterType([nameOrIndex,] type)` / `setLiquidType` | Set water, ocean/river/stagnant water, lava variants, or quicksand (`0`-`7` or name) |
| `setWaterOpacity([nameOrIndex,] opacity)` | Set opacity in the inclusive range `0`-`1` |
| `setWaterColor([nameOrIndex,] "r g b [a]")` | Set surface color and optional alpha, each in `0`-`1` |

The same setters are available as `WaterBlock::set...` methods. They return
`1` only when a matching authored water body exists and validation succeeds;
otherwise they return `0`. Water state is cleared when a mission is replaced.

### Client
| Command | Description |
|---------|-------------|
| `connect <host> [port]` | Connect to a server |
| `watchServer <host:port>` | Connect as an anonymous native UDP observer |
| `loadMission <name>` | Load a local mission |
| `playdemo <path>` | Play a .rec demo file |
| `seekDemoBlock <index>` | Seek the active demo to a block index |
| `testshape <path>` | Load a test DTS/GLB shape |
| `quit` | Exit |

Explicit demo build mode is enabled with `-demo-mode`. It does not mean
recording playback: `isDemo()` is true only for this client build path, while
`isDemoPlaying()` is true only while a `.rec` recording is playing. The
`-demo`, `--demo`, and `-playdemo` options select playback and do not enable
`isDemo()`.

Demo configuration names and defaults are:

| Name | Default | Description |
|------|---------|-------------|
| `demoMasterServer` | empty | Master URL used only in `-demo-mode`; empty selects LAN discovery |
| `demoAllowConnect` | `0` | Reject player connects in `-demo-mode` |
| `demoAllowWatch` | `1` | Permit observer connects in `-demo-mode` |

Set these in `torch.cfg`; `-demo-master-server <url>` overrides
`demoMasterServer`. An explicit master URL passed by the server browser still
wins. Normal builds use the retail master setting and allow both connection
types. The script variables `serverQuerySource` (`master` or `lan`) and
`serverQueryDemoMode` expose browser query metadata.

## Controls (default)
| Key | Action |
|-----|--------|
| WASD | Move |
| Space | Jump |
| Shift | Jet |
| Left Mouse | Fire |
| Right Mouse / R (observer) | Next follow target |
| R | Reload / Use |
| F1 | Free camera |
| F3 | Editor mode |
| Tab | Scoreboard |
| Enter | Chat |
| ~ | Console |

## Architecture

- **net/protocol.h** – UDP wire protocol with ghost replication,
  datablock sync, and game state messages
- **net/protocol.cpp** – GameServer with client management,
  authoritative movement, AI bots, and CTF/DM/Team DM game modes
- **game/game.cpp** – Client with rendering, input, ghost tracking
- **game/demo.h** – Ghost tracker, demo parser

Native observer connections do not send player movement or arbitrary remote
commands. They only send the required `setPlayerTeam 0`, `ScopeCommanderMap 1`,
and `WatchOnly ImaWatcher` setup commands; demo playback remains local and does
not require a relay or WebSocket dependency.

## License

MIT
Observer target finder: while watching a demo or live observer session, press `F3` to open the native player/flag list. Type to search, use `Up`/`Down`, press `Enter` to follow a player, and `Esc` or `F3` to close.
