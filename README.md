# Torch - Torque2D Engine Reimplementation

A cross-platform reimplementation of the Tribes 2 / Torque Game Engine
networking and gameplay core.

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
./build/torch_server -p 28000 -m test
```

Options:
- `-p <port>`  – Server port (default: 28000)
- `-m <name>`  – Mission to load (e.g. `test`, `deathmatch`)
- `-h`         – Help

## Console Commands

### Server
| Command | Description |
|---------|-------------|
| `startServer [port] [mission]` | Start a game server |
| `sv_addbot` | Spawn an AI bot |
| `sv_mission <name>` | Set mission for next start |
| `sv_gamemode <0|1>` | 0=Deathmatch, 1=Team DM |
| `sv_scorelimit <n>` | Score limit (default: 25) |
| `kick <id>` | Kick a player |
| `ban <id>` | Ban a player by IP |
| `unbanall` | Clear ban list |
| `sv_map <mission>` | Change map during game |
| `record <path>` | Start recording |
| `stoprecord` | Stop recording |

### Client
| Command | Description |
|---------|-------------|
| `connect <host> [port]` | Connect to a server |
| `playdemo <path>` | Play a .rec demo file |
| `testshape <path>` | Load a test DTS/GLB shape |
| `quit` | Exit |

## Controls (default)
| Key | Action |
|-----|--------|
| WASD | Move |
| Space | Jump |
| Shift | Jet |
| Left Mouse | Fire |
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

## License

MIT
