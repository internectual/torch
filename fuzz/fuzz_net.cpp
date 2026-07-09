// Headless libFuzzer target for the T2Protocol wire decoders.
// Exercises decodeDatablock / decodeGhostHeader / decodeGameState /
// decodeChat (and the encode paths) against untrusted input.
// Links ONLY protocol.h (inline codecs) + the Console stub — no GameServer/Engine.

#include <cstdint>
#include <cstddef>
#include <string>
#include "net/protocol.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    using namespace T2Protocol;

    // Datablock
    {
        DatablockHeader hdr{};
        const uint8_t* p = nullptr;
        size_t pl = 0;
        decodeDatablock(data, size, hdr, p, pl);
    }

    // Ghost headers — loop over a packet exactly like the real client does
    // (server batches multiple ghosts after one wire header).
    {
        GhostMessage gm{};
        const uint8_t* gp = data;
        size_t grem = size;
        while (grem > 0 && (gp[0] == GDT_Ghost || gp[0] == GDT_GhostAlways)) {
            if (!decodeGhostHeader(gp, grem, gm)) break;
            size_t hdrSize = 1 + 4 + 1 + 4;
            grem = (grem > hdrSize) ? grem - hdrSize : 0;
            gp += hdrSize;
        }
    }

    // GameState
    {
        GameStateMessage gs{};
        decodeGameState(data, size, gs);
    }

    // Chat
    {
        ChatMessage chat{};
        decodeChat(data, size, chat);
    }

    // Encode paths (round-trip sanity) — make sure the encoders don't choke
    // on attacker-shaped structs either.
    {
        ChatMessage cm{};
        cm.sender[0] = 'a'; cm.sender[1] = 0;
        cm.text[0] = 'h'; cm.text[1] = 0;
        uint8_t buf[512];
        encodeChat(buf, sizeof(buf), cm);

        GameStateMessage gsm{};
        uint8_t b2[64];
        encodeGameState(b2, sizeof(b2), gsm);

        GhostMessage gm2{};
        uint8_t b3[32];
        encodeGhostHeader(b3, sizeof(b3), gm2);
    }

    return 0;
}
