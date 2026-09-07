#include "net/v12_datablocks.h"

#include <stdexcept>

namespace {
using Stream = V12BitStream;

thread_local V12::DecodedDataBlock* activeDecoded = nullptr;

void refs(Stream& s, int n) { while (n--) { if (s.readFlag()) s.readUnsigned(11); } }
void strings(Stream& s, int n) { while (n--) s.readString(); }
void f32s(Stream& s, int n) { while (n--) s.readF32(); }
void u32s(Stream& s, int n) { while (n--) s.readUnsigned(32); }
void bools(Stream& s, int n) { while (n--) s.readUnsigned(8); }
void colors(Stream& s, int n) { while (n--) u32s(s, 1); }
void ranged(Stream& s, uint32_t max, int n) { while (n--) s.readRange(0, max); }
void rangedS32(Stream& s, int min, int max) { s.readRange(0, (uint32_t)(max - min)); }
void rangedF32(Stream& s, float min, float max, int bits) { s.readFloat(bits); }
void particle(Stream& s) {
    s.readFloat(10); if (s.readFlag()) s.readF32(); s.readSignedFloat(12);
    s.readFloat(9); if (s.readFlag()) s.readF32(); s.readUnsigned(10); s.readUnsigned(10);
    if (s.readFlag()) s.readF32();
    if (s.readFlag()) { s.readUnsigned(11); s.readUnsigned(11); }
    s.readFlag();
    const int keys = (int)s.readUnsigned(2) + 1;
    for (int i = 0; i < keys; ++i) { for (int j=0;j<4;++j)s.readFloat(7); s.readFloat(14); s.readFloat(8); }
    const int textures = (int)s.readUnsigned(6); strings(s, textures);
}
void emitter(Stream& s) {
    s.readUnsigned(10); s.readUnsigned(10); s.readUnsigned(16); s.readUnsigned(14);
    if (s.readFlag()) s.readUnsigned(16); ranged(s,180,2);
    if (s.readFlag()) ranged(s,360,1); if (s.readFlag()) ranged(s,360,1);
    for (int i=0;i<3;++i)s.readFlag(); s.readUnsigned(10);s.readUnsigned(10);s.readFlag();s.readFlag();
    const uint32_t count=s.readUnsigned(32);
    if (count > s.remainingBits()) throw std::runtime_error("invalid particle count");
    for (uint32_t i=0;i<count;++i) { if(s.readFlag())s.readUnsigned(11); }
}

void shapeBase(Stream& s) {
    if (s.readFlag()) s.readUnsigned(32);
    const std::string shape = s.readHuffmanString();
    if (activeDecoded) activeDecoded->shapeFile = shape;
    for (int i = 0; i < 9; ++i) if (s.readFlag()) s.readF32();
    const std::string debrisShape = s.readHuffmanString();
    if (activeDecoded) activeDecoded->debrisShape = debrisShape;
    if (s.readFlag()) { s.readUnsigned(10); u32s(s, 1); }
    if (s.readFlag()) s.readF32();
    if (activeDecoded) activeDecoded->cloakTexture = s.readString();
    else s.readString();
    s.readString();
    for (int i = 0; i < 6; ++i) s.readFlag();
    refs(s, 4);
    for (int i = 0; i < 3; ++i) s.readFlag();
    s.readUnsigned(32);
    if (s.readFlag()) f32s(s, 3);
    for (int i = 0; i < 8; ++i) {
        if (!s.readFlag()) continue;
        s.readHuffmanString();
        if (s.readFlag()) s.readHuffmanString();
        for (int j = 0; j < 5; ++j) s.readFlag();
    }
}

void projectile(Stream& s) {
    strings(s, 1); s.readSigned(32); s.readSigned(32); s.readFlag();
    if (s.readFlag()) f32s(s, 3);
    refs(s, 15);
    if (s.readFlag()) { s.readFloat(8); s.readFloat(7); s.readFloat(7); s.readFloat(7); }
    if (s.readFlag()) { s.readFloat(7); s.readFloat(7); s.readFloat(7); }
    s.readUnsigned(8); s.readF32();
}

void linear(Stream& s) {
    projectile(s); f32s(s, 2); u32s(s, 2); s.readFlag(); ranged(s, 90, 2); u32s(s, 2); s.readFlag();
}
void grenade(Stream& s) { projectile(s); s.readUnsigned(32); f32s(s, 6); s.readUnsigned(32); }
void shapeImage(Stream& s) {
    if (s.readFlag()) s.readUnsigned(32);
    const std::string shapeName = s.readString();
    s.readUnsigned(32);
    if (!s.readFlag()) {
        s.readPoint3F();
        s.readF32();
        s.readF32();
        s.readF32();
        s.readFlag();
    }
    s.readFlag(); s.readF32(); s.readFlag(); s.readF32(); s.readFlag(); refs(s, 2);
    if (s.readFlag()) { f32s(s, 4); s.readFlag(); s.readF32(); }
    s.readFlag();
    const uint32_t lightType = s.readRange(0, 3);
    if (lightType != 0) { s.readF32(); s.readSigned(32); for (int i=0;i<4;++i)s.readFloat(7); }
    f32s(s, 3); s.readF32(); s.readF32(); refs(s, 1); s.readFlag();
    for (int i=0;i<31;++i) {
        const bool hasState = s.readFlag();
        if (!hasState) continue;
        s.readString();
        for (int j=0;j<11;++j)s.readUnsigned(5);
        if (s.readFlag())s.readF32(); for (int j=0;j<6;++j)s.readFlag();
        if (s.readFlag()) s.readF32();
        s.readUnsigned(3); s.readUnsigned(3); s.readUnsigned(3);
        if(s.readFlag())s.readSignedInt(16); if(s.readFlag())s.readSignedInt(16);
        s.readFlag(); s.readFlag();
        const bool hasEmitter = s.readFlag();
        if (hasEmitter) { s.readUnsigned(11); s.readF32(); s.readSigned(32); }
        if (s.readFlag()) s.readUnsigned(11);
    }
}

void player(Stream& s) {
    shapeBase(s); s.readFlag(); f32s(s, 13); refs(s, 2); f32s(s, 9); s.readF32(); f32s(s, 8);
    s.readUnsigned(7); f32s(s, 6); f32s(s, 9); s.readF32(); refs(s, 32); f32s(s, 3);
    refs(s, 1); f32s(s, 2); refs(s, 1); s.readF32(); refs(s, 2); refs(s, 3); f32s(s, 11);
}

void vehicle(Stream& s) { shapeBase(s); f32s(s,2); refs(s,2); f32s(s,19); refs(s,5); refs(s,1); refs(s,3); refs(s,2); f32s(s,12); }

void effect(Stream& s, int index) {
    switch (index) {
    case 0: f32s(s,1); s.readFlag(); if (s.readFlag()) u32s(s,3); s.readFlag(); if(s.readFlag()){f32s(s,2);s.readUnsigned(9);s.readUnsigned(9);s.readFloat(6);s.readNormalVector(8);s.readF32();}s.readUnsigned(3); break;
    case 2: refs(s,3); strings(s,1); break;
    case 3: s.readFlag(); if(!s.readFlag()){for(int i=0;i<4;++i)s.readUnsigned(32);for(int i=0;i<5;++i)s.readFloat(8);}s.readFloat(8);break;
    case 4: f32s(s,6); bools(s,4); f32s(s,2); bools(s,2); f32s(s,3); bools(s,1); strings(s,2); refs(s,3); break;
    case 9: f32s(s,2); strings(s,1); break;
    case 11: f32s(s,3); bools(s,1); strings(s,1); break;
    case 22: colors(s,2); f32s(s,7); if(s.readFlag())strings(s,1); break;
    case 28: f32s(s,1); break;
    case 34: f32s(s,1); colors(s,1); f32s(s,1); strings(s,1); f32s(s,6); if(s.readFlag())strings(s,1); break;
    default: break;
    }
}
}

namespace V12 {
bool readDataBlockPayload(V12BitStream& s, size_t classId,
                          DecodedDataBlock* decoded) {
    activeDecoded = decoded;
    if (activeDecoded) *activeDecoded = {};
    const size_t i = classId >= 128 ? classId - 128 : classId;
    if (i >= 54) { activeDecoded = nullptr; return false; }
    switch (i) {
     case 0: s.readFloat(6); if (s.readFlag()) { u32s(s, 3); } if (s.readFlag()) { s.readF32(); s.readF32(); s.readUnsigned(9); s.readUnsigned(9); s.readFloat(6); s.readNormalVector(8); s.readF32(); } s.readUnsigned(3); break;
     case 1: if (s.readFlag()) s.readRange(0, 26); else { rangedS32(s,-10000,0); rangedS32(s,-10000,10000); rangedS32(s,-10000,2000); rangedF32(s,0.1f,10,8); rangedF32(s,0.1f,20,8); rangedF32(s,0.1f,20,8); rangedF32(s,0,0.3f,9); rangedF32(s,0,0.1f,7); rangedS32(s,-10000,0); rangedF32(s,0,1,9); rangedF32(s,0,2,10); rangedF32(s,1,100,8); rangedF32(s,0,1,10); s.readUnsigned(6); } rangedF32(s,0,1,8); break;
     case 2: refs(s,3); strings(s,1); break;
     case 3: rangedS32(s,-10000,1000); rangedS32(s,-10000,0); rangedS32(s,-10000,1000); rangedS32(s,-10000,0); rangedF32(s,0,1,9); rangedF32(s,0,1,8); rangedF32(s,0,1,9); rangedF32(s,0,1,8); rangedF32(s,0,10,9); rangedF32(s,0,10,9); rangedF32(s,0,10,9); rangedS32(s,-10000,0); s.readUnsigned(3); break;
     case 4: grenade(s); f32s(s,6); strings(s,2); break;
     case 5: shapeBase(s); break; case 6: strings(s,1); break; case 7: strings(s,5); break;
     case 8: f32s(s,2); u32s(s,2); f32s(s,2); bools(s,4); f32s(s,4); f32s(s,2); bools(s,2); f32s(s,3); bools(s,1); strings(s,2); refs(s,3); break;
     case 9: f32s(s,2); strings(s,1); break; case 10: projectile(s); f32s(s,6); strings(s,3); refs(s,1); break;
     case 11: f32s(s,3); s.readUnsigned(8); strings(s,1); break; case 12: grenade(s); f32s(s,7); strings(s,2); break;
     case 13: strings(s,1); refs(s,2); s.readUnsigned(14); s.readF32(); s.readFlag(); if(s.readFlag()){s.readUnsigned(16);s.readUnsigned(16);s.readUnsigned(16);} s.readUnsigned(14); ranged(s,180,2); ranged(s,360,2); ranged(s,1000,2); s.readUnsigned(14); ranged(s,10000,1); for(int j=0;j<4;++j)s.readUnsigned(16); s.readF32(); s.readFlag();s.readFlag();f32s(s,9);refs(s,11); { const int n=(int)s.readRange(0,4); for(int j=0;j<n;++j)s.readFloat(8); for(int j=0;j<n;++j){ranged(s,16000,1);ranged(s,16000,1);ranged(s,16000,1);} } break;
    case 14: refs(s,1); break; case 15: grenade(s); s.readF32(); bools(s,1); strings(s,2); break;
    case 16: vehicle(s); refs(s,6); f32s(s,16); break; case 17: f32s(s,3); s.readFlag();s.readFlag();colors(s,2);s.readUnsigned(32);s.readUnsigned(32);f32s(s,3);strings(s,5);break;
    case 18: break; case 19: grenade(s); break; case 20: vehicle(s); f32s(s,17);f32s(s,3);f32s(s,2);refs(s,7);f32s(s,3);break;
    case 21: shapeBase(s); s.readFloat(10);s.readFloat(10);s.readFlag();if(s.readFlag())s.readFloat(10);if(s.readFlag())s.readF32();if(s.readFlag()){s.readUnsigned(2);for(int j=0;j<4;++j)s.readFloat(7);s.readSigned(32);s.readF32();s.readFlag();}break;
    case 22: effect(s,22); break; case 23: refs(s,8);strings(s,8);refs(s,1);break; case 24: linear(s);s.readUnsigned(32);colors(s,1);strings(s,2);f32s(s,3);break;
    case 25: linear(s); break; case 26: shapeBase(s); break; case 27: particle(s); break;
    case 28: s.readF32(); break; case 29: emitter(s); break;
    case 30: player(s); break; case 31: refs(s,1);s.readUnsigned(32);s.readF32();strings(s,1);f32s(s,13);break; case 32: projectile(s);break; case 33: projectile(s);f32s(s,8);strings(s,2);break;
     case 34: s.readF32();colors(s,1);f32s(s,2);strings(s,1);f32s(s,6);if(s.readFlag())strings(s,1);break; case 35: projectile(s);f32s(s,10);s.readUnsigned(8);f32s(s,2);s.readUnsigned(32);s.readUnsigned(32);strings(s,2);refs(s,3);break; case 36: break; case 37: shapeBase(s);break; case 38: shapeImage(s);break;
     case 39: projectile(s);f32s(s,7);refs(s,1);f32s(s,8);strings(s,4);refs(s,1);break; case 40: f32s(s,15);bools(s,5);refs(s,3);colors(s,4);f32s(s,4);strings(s,2);break; case 41: break; case 42: projectile(s);f32s(s,11);colors(s,2);f32s(s,1);strings(s,12);break; case 43: f32s(s,3);u32s(s,4);s.readF32();s.readUnsigned(32);f32s(s,9);refs(s,4);colors(s,4);f32s(s,4);strings(s,2);break;
     case 44: shapeBase(s);s.readFlag();s.readUnsigned(32);break; case 45: f32s(s,10);strings(s,4);break; case 46: f32s(s,11);colors(s,1);f32s(s,6);strings(s,11);break; case 47: strings(s,1);strings(s,(int)s.readUnsigned(7));break; case 48: projectile(s);s.readF32();colors(s,1);f32s(s,7);strings(s,4);break; case 49: linear(s);f32s(s,3);bools(s,1);colors(s,1);f32s(s,2);bools(s,1);strings(s,2);break; case 50:s.readUnsigned(32);break; case 51:shapeBase(s);s.readFlag();s.readUnsigned(32);f32s(s,3);s.readFlag();ranged(s,3,1);f32s(s,2);break; case 52:shapeImage(s);s.readUnsigned(8);s.readUnsigned(8);ranged(s,1080,2);s.readFlag();s.readF32();s.readFlag();break; case 53:vehicle(s);f32s(s,9);refs(s,5);f32s(s,11);break;
    }
    const bool ok = !s.failed();
    activeDecoded = nullptr;
    return ok;
}
}
