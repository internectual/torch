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
- [x] DSO bytecode reader and full VM opcode dispatch (~60 opcodes)
- [x] TorqueScript interpreter (parser, locals, functions, natives)
- [x] OpenGL 3.3 renderer with terrain, skybox, text, DTS models, DIF interiors
- [x] OpenAL 3D audio (WAV playback)
- [x] UDP networking with T2 protocol constants
- [x] Player physics (gravity, jet, jump)
- [x] HUD and menu system
- [x] Weapon system (spinfusor, blaster, chaingun, grenade launcher, sniper, mortar)
- [x] Collision detection (ray-triangle, sphere-triangle, spatial grid)
- [x] DTS model loader with skeletal animation
- [x] DIF interior loader with BSP, surfaces, lightmaps, materials
- [x] Demo playback (BitStream, Huffman, GhostTracker, full parser)
- [x] Mission parser (.mis files with Terrain, Sky, Sun, interiors, items)
- [x] GUI shell rendering (bitmap buttons, text fields, panes, titlebars, checkboxes, sliders, scrollbars)
- [x] BM8 texture format and 3-slice/9-slice bitmap border rendering
- [x] GFT font loader with proportional glyphs (Arial, Verdana, Lucida Console, Sui Generis)
- [x] Profile-aware control rendering (fillColor, fontColor, textOffset, bitmap)
- [x] TorqueScript object tree viewer and property inspector
- [x] Console with scroll, history, input, and AI IPC (@ prefix)
- [x] Dev panel with editable init path, args, log, and tabbed bottom panel
- [x] Preload system, -exec and -compile CLI flags
- [ ] TribesNext RSA authentication
