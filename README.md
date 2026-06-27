# Torch

**Torque Open (Re)source Client Hack** — a script, protocol, and bytecode compatible modern Linux client for Tribes 2 using OpenGL 3.3 Core and SDL3.

## Build

```bash
cmake -B build
cmake --build build -j$(nproc)
./build/torch -data /path/to/tribes2
```

## Dependencies

- SDL3
- OpenGL 3.3+
- GLEW
- OpenAL
- GLU
- Zlib

## Status

- [x] SDL3 window + OpenGL 3.3 core context
- [x] VOL/VL2 archive reader (T2 asset loading)
- [x] DSO bytecode reader and stack VM
- [x] TorqueScript console system (vars, commands, exec)
- [x] OpenGL 3.3 renderer with terrain, skybox, text
- [x] OpenAL 3D audio (WAV playback)
- [x] UDP networking with T2 protocol constants
- [x] Player physics (gravity, jet, jump)
- [x] HUD and menu system
- [ ] Full bytecode VM opcode dispatch
- [ ] TribesNext RSA authentication
- [ ] DTS/DIF model/map loaders
