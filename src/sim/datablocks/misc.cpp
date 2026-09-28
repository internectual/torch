// DataBlockPack group: see src/sim/datablock_pack.h.
//
// Audio (audio/audioDataBlock.cc), SensorData, TriggerData, ForceFieldBareData,
// CommanderIconData, CannedChatItem, GameBaseData, SimDataBlock and
// TSShapeConstructor, in the layout retail Tribes 2 writes (checked against
// the retail Linux binary where it differs from the V12 source) and Torch's
// reader consumes.
#include "sim/datablock_pack.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <strings.h>

namespace DataBlockPack {

namespace {

// audioDataBlock.cc helpers. The float is truncated (fistp with the
// round-toward-zero control word in retail).
void writeRangedF32(TorqueBitWriter& w, float value, float min, float max, int bits) {
    const float normalized = (std::clamp(value, min, max) - min) / (max - min);
    w.writeInt((int32_t)(normalized * (float)((1 << bits) - 1)), bits);
}

// writeRangedU32(val - min, 0, max - min). The engine asserts on values out of
// range; clamp so an out-of-range script value cannot spill into other bits.
void writeRangedS32(TorqueBitWriter& w, int32_t value, int32_t min, int32_t max) {
    w.writeRangedU32((uint32_t)(std::clamp(value, min, max) - min), 0, (uint32_t)(max - min));
}

// A string field that may be set either as "name[i]" or "namei" (the
// TorqueScript array form and the engine's flattened field name).
bool hasIndexed(const Context& c, const std::string& base, int i) {
    return c.has((base + "[" + std::to_string(i) + "]").c_str()) ||
           c.has((base + std::to_string(i)).c_str());
}
std::string indexedStr(const Context& c, const std::string& base, int i) {
    const std::string bracket = base + "[" + std::to_string(i) + "]";
    if (c.has(bracket.c_str())) return c.str(bracket.c_str());
    return c.str((base + std::to_string(i)).c_str());
}

// AudioEnvironment 'room' (roomEnums: AL_ENVIRONMENT_GENERIC = 0 ... PSYCHOTIC = 25).
int roomValue(const Context& c) {
    const int named = c.enumValue("room",
        {"GENERIC", "PADDEDCELL", "ROOM", "BATHROOM", "LIVINGROOM", "STONEROOM",
         "AUDITORIUM", "CONCERTHALL", "CAVE", "ARENA", "HANGAR", "CARPETEDHALLWAY",
         "HALLWAY", "STONECORRIDOR", "ALLEY", "FOREST", "CITY", "MOUNTAINS", "QUARRY",
         "PLAIN", "PARKINGLOT", "SEWERPIPE", "UNDERWATER", "DRUGGED", "DIZZY", "PSYCHOTIC"},
        -1);
    if (named >= 0) return named;
    // TypeEnum falls back to the numeric value.
    return std::clamp(c.s32("room", 0), 0, 26);
}

void audioDescription(Context& c) {
    auto& w = c.w;
    // Constructor defaults, then AudioDescription::onAdd's validation.
    const float volume = std::clamp(c.f32("volume", 1.0f), 0.0f, 1.0f);
    const bool looping = c.boolean("isLooping", false);
    const bool is3d = c.boolean("is3D", false);
    int32_t loopCount = c.s32("loopCount", -1);
    int32_t minLoopGap = c.s32("minLoopGap", 0);
    int32_t maxLoopGap = c.s32("maxLoopGap", 0);
    loopCount = std::max(loopCount, -1);
    maxLoopGap = std::max(maxLoopGap, minLoopGap);
    minLoopGap = std::clamp(minLoopGap, 0, maxLoopGap);
    float minDistance = c.f32("minDistance", 1.0f);
    float maxDistance = c.f32("maxDistance", 100.0f);
    int32_t coneInside = c.s32("coneInsideAngle", 360);
    int32_t coneOutside = c.s32("coneOutsideAngle", 360);
    float coneOutsideVolume = c.f32("coneOutsideVolume", 1.0f);
    std::array<float, 3> coneVector = c.point("coneVector", {0, 0, 1});
    float environmentLevel = c.f32("environmentLevel", 0.0f);
    if (is3d) {
        minDistance = std::max(minDistance, 0.0f);
        maxDistance = maxDistance > minDistance ? maxDistance : minDistance + 0.01f;
        coneInside = std::clamp(coneInside, 0, 360);
        coneOutside = std::clamp(coneOutside, coneInside, 360);
        coneOutsideVolume = std::clamp(coneOutsideVolume, 0.0f, 1.0f);
        const float len = std::sqrt(coneVector[0] * coneVector[0] + coneVector[1] * coneVector[1] +
                                    coneVector[2] * coneVector[2]);
        if (len > 0.0f) for (float& v : coneVector) v /= len;
        environmentLevel = std::clamp(environmentLevel, 0.0f, 1.0f);
    }
    int32_t type = c.s32("type", 0);
    if (type < 0 || type >= 6) type = 0; // Audio::NumAudioTypes -> DefaultAudioType

    w.writeFloat(volume, 6);
    if (w.writeFlag(looping)) {
        w.writeInt(loopCount, 32);
        w.writeInt(minLoopGap, 32);
        w.writeInt(maxLoopGap, 32);
    }
    if (w.writeFlag(is3d)) {
        w.writeF32(minDistance);
        w.writeF32(maxDistance);
        w.writeInt(coneInside, 9);
        w.writeInt(coneOutside, 9);
        // writeInt(mConeOutsideVolume, 6): the F32 is truncated to an integer
        // (V12 source and retail binary alike), so only 0 or 1 go out.
        w.writeInt((int32_t)coneOutsideVolume, 6);
        w.writeNormalVector({coneVector[0], coneVector[1], coneVector[2]}, 8);
        w.writeF32(environmentLevel);
    }
    w.writeInt(type, 3);
}

void audioProfile(Context& c) {
    // Retail: description (TypeAudioDescriptionPtr), effect (EffectProfile),
    // environment (TypeAudioSampleEnvironmentPtr), then the filename less ".wav".
    c.writeRef(c.ref("description"));
    c.writeRef(c.ref("effect"));
    c.writeRef(c.ref("environment"));
    std::string filename = c.str("filename").substr(0, 255);
    if (filename.size() > 3 && strcasecmp(filename.c_str() + filename.size() - 4, ".wav") == 0)
        filename.resize(filename.size() - 4);
    c.w.writeString(filename);
}

void audioEnvironment(Context& c) {
    auto& w = c.w;
    if (w.writeFlag(c.boolean("useRoom", true))) {
        w.writeRangedU32((uint32_t)roomValue(c), 0, 26);
    } else {
        writeRangedS32(w, c.s32("roomHF", 0), -10000, 0);
        writeRangedS32(w, c.s32("reflections", 0), -10000, 10000);
        writeRangedS32(w, c.s32("reverb", 0), -10000, 2000);
        writeRangedF32(w, c.f32("roomRolloffFactor", 0.1f), 0.1f, 10.0f, 8);
        writeRangedF32(w, c.f32("decayTime", 0.1f), 0.1f, 20.0f, 8);
        writeRangedF32(w, c.f32("decayHFRatio", 0.1f), 0.1f, 20.0f, 8);
        writeRangedF32(w, c.f32("reflectionsDelay", 0.0f), 0.0f, 0.3f, 9);
        writeRangedF32(w, c.f32("reverbDelay", 0.0f), 0.0f, 0.1f, 7);
        writeRangedS32(w, c.s32("roomVolume", 0), -10000, 0);
        // Retail moved effectVolume out of this branch (written last, below);
        // the ranges are the retail binary's (same as V12).
        writeRangedF32(w, c.f32("damping", 0.0f), 0.0f, 2.0f, 9);
        writeRangedF32(w, c.f32("environmentSize", 10.0f), 1.0f, 100.0f, 10);
        writeRangedF32(w, c.f32("environmentDiffusion", 1.0f), 0.0f, 1.0f, 8);
        writeRangedF32(w, c.f32("airAbsorption", 0.0f), -100.0f, 0.0f, 10);
        w.writeInt(c.s32("flags", 0), 6);
    }
    writeRangedF32(w, c.f32("effectVolume", 0.0f), 0.0f, 1.0f, 8);
}

void audioSampleEnvironment(Context& c) {
    auto& w = c.w;
    writeRangedS32(w, c.s32("direct", 0), -10000, 1000);
    writeRangedS32(w, c.s32("directHF", 0), -10000, 0);
    writeRangedS32(w, c.s32("room", 0), -10000, 1000);
    writeRangedS32(w, c.s32("roomHF", 0), -10000, 0);
    writeRangedF32(w, c.f32("obstruction", 0.0f), 0.0f, 1.0f, 9);
    writeRangedF32(w, c.f32("obstructionLFRatio", 0.0f), 0.0f, 1.0f, 8);
    writeRangedF32(w, c.f32("occlusion", 0.0f), 0.0f, 1.0f, 9);
    writeRangedF32(w, c.f32("occlusionLFRatio", 0.0f), 0.0f, 1.0f, 8);
    writeRangedF32(w, c.f32("occlusionRoomRatio", 0.0f), 0.0f, 10.0f, 9);
    writeRangedF32(w, c.f32("roomRolloff", 0.0f), 0.0f, 10.0f, 9);
    writeRangedF32(w, c.f32("airAbsorption", 0.0f), 0.0f, 10.0f, 9);
    writeRangedS32(w, c.s32("outsideVolumeHF", 0), -10000, 0);
    w.writeInt(c.s32("flags", 0), 3);
}

void forceFieldBare(Context& c) {
    auto& w = c.w;
    // ForceFieldBareData::onAdd clamps fadeMS to [0, 10000] and
    // baseTranslucency to [0, 1].
    w.writeInt(std::clamp(c.s32("fadeMS", 1000), 0, 10000), 32);
    w.writeF32(std::clamp(c.f32("baseTranslucency", 0.65f), 0.0f, 1.0f));
    w.writeF32(c.f32("powerOffTranslucency", 0.35f));
    w.writeFlag(c.boolean("teamPermiable", false));
    w.writeFlag(c.boolean("otherPermiable", false));
    c.writeColorI(c.colorF("color", {0.5f, 0.5f, 1.0f, 1.0f}));
    c.writeColorI(c.colorF("powerOffColor", {0.25f, 0.0f, 0.0f, 1.0f}));
    w.writeInt(c.s32("framesPerSec", 10), 32);
    w.writeInt(c.s32("numFrames", 5), 32); // NUM_TEX
    w.writeF32(c.f32("scrollSpeed", 0.5f));
    w.writeF32(c.f32("umapping", 1.0f));
    w.writeF32(c.f32("vmapping", 1.0f));
    for (int i = 0; i < 5; ++i) w.writeString(indexedStr(c, "texture", i));
}

void commanderIcon(Context& c) {
    // mImageDesc[NumImages]: "images[i]" or the named aliases.
    static const char* const names[] = {"baseImage", "activeImage", "inactiveImage",
                                        "selectImage", "hilightImage"};
    for (int i = 0; i < 5; ++i) {
        std::string value;
        if (c.has(names[i])) value = c.str(names[i]);
        else value = indexedStr(c, "images", i);
        c.w.writeString(value);
    }
}

void tsShapeConstructor(Context& c) {
    constexpr int MaxSequences = 127; // (1 << NumSequenceBits) - 1
    c.w.writeString(c.str("baseShape"));
    int count = 0;
    for (int i = 0; i < MaxSequences; ++i)
        if (hasIndexed(c, "sequence", i)) ++count;
    c.w.writeInt(count, 7);
    for (int i = 0; i < MaxSequences; ++i)
        if (hasIndexed(c, "sequence", i)) c.w.writeString(indexedStr(c, "sequence", i));
}

} // namespace

void registerMisc() {
    registerClass("AudioDescription", audioDescription);
    registerClass("AudioProfile", audioProfile);
    registerClass("AudioEnvironment", audioEnvironment);
    registerClass("AudioSampleEnvironment", audioSampleEnvironment);
    registerClass("SensorData", [](Context&) {});   // SensorData::packData writes nothing
    registerClass("TriggerData", [](Context& c) { c.w.writeInt(c.s32("tickPeriodMS", 100), 32); });
    registerClass("ForceFieldBareData", forceFieldBare);
    registerClass("CommanderIconData", commanderIcon);
    // The retail binary's CannedChatItem vtable uses SimDataBlock::packData.
    registerClass("CannedChatItem", [](Context&) {});
    registerClass("GameBaseData", [](Context&) {});
    registerClass("SimDataBlock", [](Context&) {});
    registerClass("TSShapeConstructor", tsShapeConstructor);
}

} // namespace DataBlockPack
