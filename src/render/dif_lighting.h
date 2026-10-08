#pragma once
// Interior animated lights and alarm lighting (Interior::mAnimatedLights,
// mLightStates, mStateData, mStateDataBuffer; FUN_00525d90/00525fc0/005260c0
// sample, FUN_00524ec0/005263f0 relight), as ported by t2-mapper
// difLighting.ts and scene/interiorAlarm.ts. Pure data and math; the GL
// side is DIFLightingInstance (dif_lighting.cpp).
#include "game/timeline_random.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

// Animated light flags: the type is flags & 7.
enum DIFLightFlag : uint16_t {
    DIFLight_Ambient = 1,
    DIFLight_Loop = 2,
    DIFLight_Flicker = 4,
    DIFLight_Alarm = 8,
};

// The relight runs on 66 ms buckets of the lighting clock.
inline constexpr int DIFLightUpdateMs = 66;
inline constexpr uint32_t DIFNoStateMap = 0xffffffffu;
inline constexpr uint8_t DIFNoLightmap = 0xff;

struct DIFAnimatedLight {
    uint32_t nameIndex = 0;
    uint32_t stateIndex = 0;
    uint16_t stateCount = 0;
    uint16_t flags = 0;
    uint32_t duration = 0; // ms
};

struct DIFLightState {
    uint8_t color[3] = {0, 0, 0};
    uint32_t activeTime = 0; // ms
    uint32_t dataIndex = 0;
    uint16_t dataCount = 0;
};

struct DIFLightStateData {
    uint32_t surfaceIndex = 0;
    uint32_t mapIndex = DIFNoStateMap; // offset into the state buffer
    uint16_t lightStateIndex = 0;
};

// The surface fields the relight needs (Interior::Surface).
struct DIFLitSurface {
    uint16_t lightCount = 0;
    uint32_t lightStateInfoStart = 0;
    uint8_t mapOffsetX = 0, mapOffsetY = 0;
    uint8_t mapSizeX = 0, mapSizeY = 0;
};

struct DIFLightSample {
    int state = -1;
    uint8_t color[3] = {0, 0, 0};
};

// One light's state and colour at `timeMs` of the lighting clock. Flicker
// uses the repeatable cosmetic RNG seeded by the light index; the
// executable's process-wide random seed is not recorded.
inline void sampleDIFLight(const DIFAnimatedLight& light, const std::vector<DIFLightState>& states,
                           double timeMs, uint32_t seed, DIFLightSample& out) {
    const uint16_t type = light.flags & 7;
    double time = std::max(0.0, timeMs);
    if (!(light.flags & (DIFLight_Ambient | DIFLight_Alarm))) time = 0.0;
    if (type & DIFLight_Flicker) {
        const size_t next = (size_t)light.stateIndex + 1;
        const double period = next < states.size() ? states[next].activeTime : 0.0;
        const double step = period > 0.0 ? std::floor(time / period) : 0.0;
        if (step > 0.0) {
            TimelineRandom random = timelineRandom({(double)seed, step});
            time = std::floor(random.next() * ((double)light.duration + 1.0));
        } else {
            time = 0.0;
        }
    } else if (type & DIFLight_Loop) {
        time = light.duration > 0 ? std::fmod(time, (double)light.duration) : 0.0;
    } else {
        time = std::min(time, (double)light.duration);
    }
    int index = 0;
    while (index + 1 < light.stateCount &&
           states[light.stateIndex + index + 1].activeTime <= time)
        ++index;
    out.state = index;
    const DIFLightState& current = states[light.stateIndex + index];
    const int nextIndex = index + 1 < light.stateCount ? index + 1 : ((type & DIFLight_Loop) ? 0 : index);
    const DIFLightState& following = states[light.stateIndex + nextIndex];
    const double end = index + 1 < light.stateCount ? following.activeTime : light.duration;
    const double fraction = !(type & DIFLight_Flicker) && end > current.activeTime
        ? std::min(1.0, (time - current.activeTime) / (end - current.activeTime)) : 0.0;
    for (int c = 0; c < 3; ++c)
        out.color[c] = (uint8_t)std::floor(
            current.color[c] + (following.color[c] - current.color[c]) * fraction + 0.5);
}

// A light slot of a surface: which light drives it and its intensity map
// in the state buffer for each of the light's states.
struct DIFLightSlot {
    uint32_t light = 0;
    std::vector<uint32_t> maps;
};

// The surfaces of one lightmap lit by animated lights of one mode (normal
// or alarm), and the lights involved.
struct DIFLightingPlan {
    struct Surface {
        int x = 0, y = 0, width = 0, height = 0;
        std::vector<DIFLightSlot> slots;
    };
    std::vector<Surface> surfaces;
    std::vector<uint32_t> lights;
};

inline int difLightingPlanKey(int lightmap, bool alarm) { return lightmap * 2 + (alarm ? 1 : 0); }

// An interior's animated lighting, shared by all its instances.
struct DIFLightingData {
    std::vector<DIFAnimatedLight> lights;
    std::vector<DIFLightState> states;
    std::vector<uint8_t> stateBuffer;
    std::map<int, DIFLightingPlan> plans; // by difLightingPlanKey
    // Decoded base images of the planned lightmaps (RGBA), by lightmap index.
    struct Pixels { int width = 0, height = 0; std::vector<uint8_t> rgba; };
    std::map<int, Pixels> basePixels;
    bool hasAlarmState = false;
    // Instances need their own lighting state.
    bool animated() const { return hasAlarmState || !plans.empty(); }
};

// Validates the animated light tables against each other, the surfaces and
// the state buffer. False names the first bad reference in `error`.
inline bool validateDIFLighting(const std::vector<DIFAnimatedLight>& lights,
                                const std::vector<DIFLightState>& states,
                                const std::vector<DIFLightStateData>& stateData,
                                size_t stateBufferSize,
                                const std::vector<DIFLitSurface>& surfaces,
                                const char*& error) {
    for (const auto& light : lights) {
        if (!light.stateCount) { error = "animated light has no states"; return false; }
        if ((size_t)light.stateIndex + light.stateCount > states.size()) {
            error = "animated light state range"; return false;
        }
    }
    for (const auto& state : states)
        if ((size_t)state.dataIndex + state.dataCount > stateData.size()) {
            error = "light state data range"; return false;
        }
    for (const auto& data : stateData) {
        if (data.surfaceIndex >= surfaces.size()) { error = "light state surface"; return false; }
        const DIFLitSurface& surface = surfaces[data.surfaceIndex];
        if (data.mapIndex != DIFNoStateMap &&
            (size_t)data.mapIndex + (size_t)surface.mapSizeX * surface.mapSizeY > stateBufferSize) {
            error = "light state map range"; return false;
        }
    }
    return true;
}

// Compiles the file-order state installations once per interior: exported
// assets can write the same slot many times, the last one wins. Keyed by
// difLightingPlanKey(lightmap, alarm); a surface joins the plan of the
// lightmap it uses in a mode only through lights of that mode.
inline std::map<int, DIFLightingPlan> buildDIFLightingPlans(
        const std::vector<DIFAnimatedLight>& lights,
        const std::vector<DIFLightState>& states,
        const std::vector<DIFLightStateData>& stateData,
        const std::vector<DIFLitSurface>& surfaces,
        const std::vector<uint8_t>& normalLightmaps,
        const std::vector<uint8_t>& alarmLightmaps,
        bool hasAlarmState) {
    std::map<uint32_t, DIFLightSlot> slots;
    for (uint32_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
        const DIFAnimatedLight& light = lights[lightIndex];
        for (uint32_t stateIndex = 0; stateIndex < light.stateCount; ++stateIndex) {
            const DIFLightState& state = states[light.stateIndex + stateIndex];
            for (uint32_t i = state.dataIndex; i < state.dataIndex + state.dataCount; ++i) {
                const DIFLightStateData& data = stateData[i];
                auto it = slots.find(data.lightStateIndex);
                if (it == slots.end())
                    it = slots.emplace(data.lightStateIndex,
                        DIFLightSlot{lightIndex, std::vector<uint32_t>(light.stateCount, DIFNoStateMap)}).first;
                if (stateIndex < it->second.maps.size()) it->second.maps[stateIndex] = data.mapIndex;
            }
        }
    }
    std::map<int, DIFLightingPlan> plans;
    for (size_t surfaceIndex = 0; surfaceIndex < surfaces.size(); ++surfaceIndex) {
        const DIFLitSurface& surface = surfaces[surfaceIndex];
        if (!surface.lightCount) continue;
        for (bool alarm : {false, true}) {
            if (alarm && !hasAlarmState) continue;
            const std::vector<uint8_t>& indices = alarm ? alarmLightmaps : normalLightmaps;
            if (surfaceIndex >= indices.size() || indices[surfaceIndex] == DIFNoLightmap) continue;
            std::vector<DIFLightSlot> contributions;
            for (uint32_t i = 0; i < surface.lightCount; ++i) {
                auto slot = slots.find(surface.lightStateInfoStart + i);
                if (slot != slots.end() &&
                    ((lights[slot->second.light].flags & DIFLight_Alarm) != 0) == alarm)
                    contributions.push_back(slot->second);
            }
            if (contributions.empty()) continue;
            DIFLightingPlan& plan = plans[difLightingPlanKey(indices[surfaceIndex], alarm)];
            for (const DIFLightSlot& slot : contributions)
                if (std::find(plan.lights.begin(), plan.lights.end(), slot.light) == plan.lights.end())
                    plan.lights.push_back(slot.light);
            plan.surfaces.push_back({surface.mapOffsetX, surface.mapOffsetY,
                                     surface.mapSizeX, surface.mapSizeY, std::move(contributions)});
        }
    }
    return plans;
}

// Restores each planned surface's base patch, then adds every slot's
// colour x intensity as saturating bytes in gamma space. `base` and `out`
// are RGBA rows of `width` pixels; `samples` is indexed by light.
inline void compositeDIFLightmap(const DIFLightingPlan& plan,
                                 const std::vector<DIFLightSample>& samples,
                                 const std::vector<uint8_t>& stateBuffer,
                                 const uint8_t* base, uint8_t* out, int width) {
    for (const auto& surface : plan.surfaces) {
        for (int row = 0; row < surface.height; ++row) {
            const size_t start = ((size_t)(surface.y + row) * width + surface.x) * 4;
            std::copy(base + start, base + start + (size_t)surface.width * 4, out + start);
        }
        for (const DIFLightSlot& slot : surface.slots) {
            const DIFLightSample& sample = samples[slot.light];
            if (sample.state < 0 || sample.state >= (int)slot.maps.size()) continue;
            const uint32_t map = slot.maps[sample.state];
            if (map == DIFNoStateMap) continue;
            for (int row = 0; row < surface.height; ++row) {
                uint8_t* p = out + ((size_t)(surface.y + row) * width + surface.x) * 4;
                const uint8_t* m = stateBuffer.data() + map + (size_t)row * surface.width;
                for (int col = 0; col < surface.width; ++col, p += 4, ++m)
                    for (int c = 0; c < 3; ++c)
                        p[c] = (uint8_t)std::min(255, p[c] + ((sample.color[c] * *m + 128) >> 8));
            }
        }
    }
}

// The interior's lighting clocks (scene/interiorAlarm.ts): the alarm clock
// runs only while the alarm is on; the normal clock is the rest of the time
// since the interior appeared. InteriorInstance::setAlarmMode ignores
// requests for resources without an alarm state (FUN_00524c60), so callers
// only switch on an interior that has one.
struct DIFLightingClock {
    bool alarm = false;
    double startSec = 0.0, alarmTimeSec = 0.0, alarmChangedAtSec = 0.0;

    double alarmTime(double nowSec) const {
        return alarmTimeSec + (alarm ? std::max(0.0, nowSec - alarmChangedAtSec) : 0.0);
    }
    // Time spent in the current lighting mode.
    double lightingTime(double nowSec) const {
        const double elapsed = std::max(0.0, nowSec - startSec);
        const double inAlarm = alarmTime(nowSec);
        return alarm ? inAlarm : std::max(0.0, elapsed - inAlarm);
    }
    bool setAlarm(bool on, double nowSec) {
        if (on == alarm) return false;
        alarmTimeSec = alarmTime(nowSec);
        alarm = on;
        alarmChangedAtSec = nowSec;
        return true;
    }
};
