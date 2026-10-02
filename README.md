# TORCH - Torque Open (Re)Source Client Hack

Torch is a cross-platform reimplementation of the Tribes 2 / Torque Game
Engine client, dedicated server, renderer, scripting runtime, networking,
physics, AI, audio, and demo playback systems.

Runtime assets come exclusively from an untouched Tribes 2 installation. Torch
does not use generated replacement assets, converted models, or substitute
textures. Unsupported native assets fail explicitly so rendering bugs can be
fixed against the original DTS, DIF, TER, GUI, and script data.

## Requirements

- CMake 3.20 or newer
- C++20 compiler (GCC 11+ or Clang 14+)
- SDL3, GLEW, OpenAL, GLU, zlib, libcurl, libvorbis, libmpg123, and libgsm

## Build

### Linux

```sh
sudo apt install cmake g++ libsdl3-dev libglew-dev libopenal-dev \
                 libglu1-mesa-dev libvorbis-dev libmpg123-dev \
                 libcurl4-openssl-dev libgsm1-dev zlib1g-dev
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
brew install cmake sdl3 glew openal-soft glm zlib libvorbis libmpg123 gsm
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
```

## Running

### Client

```sh
./build/torch
```

The client accepts `-data <dir>` for the Tribes 2 installation, `-mod <path>`
for the active mod, and `-output <dir>` for writable runtime state and logs.
The default output directory is `~/.torch`. Configuration is read from
`torch.cfg`, then command-line options; later console and script writes are
runtime-only overrides.

Useful client options:

- `-demo <file.rec>`, `--demo`, `-playdemo` - Play a demo recording
- `-demo-mode` - Run the stock scripts in Tribes 2 Demo mode, not playback
- `-preview <map>` - Load a map and take a screenshot
- `-mapper <map>` - Load a mission for free-fly inspection
- `-mapper-camera <n>` - Select an authored Observer camera
- `-testshape <path>` - Load a native DTS shape
- `-testdif <path>` - Dump DIF interior statistics
- `-exec <file>` or `-e <file>` - Execute a TorqueScript file after startup
- `-quit-after-frames <n>` - Stop after a bounded number of rendered frames
- `-h` or `--help` - Show the complete option list

The client writes `console.log` under the output directory and emits
`TORCH-RUN-START` on stderr when the main loop begins.

### Dedicated Server

```sh
./build/torch_server -mission TWL_Minotaur CTF
```

`torch_server` is the retail DedicatedServer launch (`console_start.cs`
`-dedicated`) without a window. The stock server scripts host the mission, the
port is `$Host::Port` from the server preferences, and TorqueScript is read
from stdin (`quit();` stops the server). Clients join with the stock
`JoinGame("host:port")` / `connect()` calls.

Server options include `-mission <name> <type>`, `-serverprefs <file>`,
`-mod <dir>`, `-data <dir>`, and `-output <dir>`. Use `-h` for the complete
server help.

## Console Commands

### Client

| Command | Description |
|---------|-------------|
| `connect("host:port")` | Connect to a server |
| `watchServer <host:port>` | Connect as an anonymous native UDP observer |
| `loadMission <name>` | Load a local mission |
| `playdemo <path>` | Play a `.rec` demo file |
| `seekDemoBlock <index>` | Seek the active demo to a block index |
| `testshape <path>` | Load a native DTS shape |
| `quit` | Exit |

Native observer connections send only the required observer setup commands,
not player movement or arbitrary remote commands. Demo playback is local and
does not require a relay or WebSocket dependency.

Explicit demo build mode is enabled with `-demo-mode`; it does not mean
recording playback. `isDemo()` is true only for this client build path, while
`isDemoPlaying()` is true only while a `.rec` recording is playing.

### Server

Server administration is provided by the stock scripts (`server.cs` and
`admin.cs`) through the dedicated server console. TorqueScript is read from
standard input.

## Controls

| Key | Action |
|-----|--------|
| WASD | Move or fly the free camera |
| Mouse | Look around |
| Left Mouse | Fire |
| Right Mouse | Alt fire; cycle observer targets in demo/observer mode |
| Space | Jump or jet |
| Shift | Jet or increase mapper camera speed |
| R | Reload/use; cycle observer targets in demo/observer mode |
| F1 | Free camera toggle |
| F3 | Observer target finder |
| Tab | Scoreboard |
| Enter | Chat |
| `~` | Console |
| `1`-`3` | Select authored mapper camera |
| `Esc` | Pause or quit |

In the observer target finder, type to search, use Up/Down to select a target,
press Enter to follow it, and press Esc or F3 to close the finder.

## Architecture

- **src/core** - Engine lifecycle, platform services, timing, and console
- **src/fs** - Original asset, archive, and path-policy handling
- **src/script** - TorqueScript execution, native commands, and DSO support
- **src/sim** - Server and client simulation, objects, physics, and networking
- **src/ai** - Navigation graphs, path search, bot tasks, and movement
- **src/render** - DTS/DIF/terrain loading, OpenGL rendering, GUI, and shadows
- **src/audio** - OpenAL effects and MP3 music streaming
- **src/net** - V12 protocol, datablocks, ghosts, demos, and server queries
- **src/game** - Client loop, HUD, weapons, missions, and demo playback

The dedicated server uses the same simulation and scripting systems without
the client renderer or audio presentation.

## License

MIT
