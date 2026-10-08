#include "render/dif_lighting_instance.h"
#include <GL/glew.h>
#include <algorithm>
#include <cmath>

DIFLightingInstance::DIFLightingInstance(double startSec) {
    clock.startSec = startSec;
    clock.alarmChangedAtSec = startSec;
}

DIFLightingInstance::~DIFLightingInstance() {
    for (auto& [key, atlas] : atlases) atlas.texture.destroy();
}

bool DIFLightingInstance::setAlarm(const DTSShape& shape, bool alarm, double nowSec) {
    return clock.setAlarm(alarm && shape.interiorLighting.hasAlarmState, nowSec);
}

uint32_t DIFLightingInstance::animatedLightmap(int lightmap) const {
    auto it = atlases.find(difLightingPlanKey(lightmap, clock.alarm));
    return it != atlases.end() && it->second.texture.loaded ? it->second.texture.id : 0;
}

void DIFLightingInstance::prepare(const DTSShape& shape, double nowSec) {
    const DIFLightingData& data = shape.interiorLighting;
    if (data.plans.empty()) return;
    if (samples.size() != data.lights.size()) {
        samples.assign(data.lights.size(), {});
        sampleBuckets.assign(data.lights.size(), INT64_MIN);
        sampleVersions.assign(data.lights.size(), 0);
    }
    const int64_t bucket = (int64_t)std::floor(
        std::max(0.0, clock.lightingTime(nowSec) * 1000.0) / DIFLightUpdateMs);
    for (const auto& [key, plan] : data.plans) {
        if ((key & 1) != (clock.alarm ? 1 : 0)) continue;
        auto base = data.basePixels.find(key / 2);
        if (base == data.basePixels.end()) continue;
        Atlas& atlas = atlases[key];
        if (!atlas.texture.loaded) {
            atlas.rgba = base->second.rgba;
            atlas.texture.loadRaw(atlas.rgba.data(), base->second.width, base->second.height, 4);
            atlas.versions.assign(plan.lights.size(), UINT32_MAX);
            if (!atlas.texture.loaded) continue;
        }
        if (bucket == atlas.bucket) continue;
        atlas.bucket = bucket;
        bool changed = false;
        for (size_t i = 0; i < plan.lights.size(); ++i) {
            const uint32_t light = plan.lights[i];
            if (sampleBuckets[light] != bucket) {
                const DIFLightSample previous = samples[light];
                sampleDIFLight(data.lights[light], data.states,
                               (double)bucket * DIFLightUpdateMs, light, samples[light]);
                sampleBuckets[light] = bucket;
                if (previous.state != samples[light].state ||
                    !std::equal(previous.color, previous.color + 3, samples[light].color))
                    ++sampleVersions[light];
            }
            if (atlas.versions[i] != sampleVersions[light]) changed = true;
            atlas.versions[i] = sampleVersions[light];
        }
        if (!changed) continue;
        const int width = base->second.width;
        compositeDIFLightmap(plan, samples, data.stateBuffer, base->second.rgba.data(),
                             atlas.rgba.data(), width);
        // Upload only the relit surface patches.
        glBindTexture(GL_TEXTURE_2D, atlas.texture.id);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
        for (const auto& surface : plan.surfaces) {
            if (surface.width <= 0 || surface.height <= 0) continue;
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, surface.x);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, surface.y);
            glTexSubImage2D(GL_TEXTURE_2D, 0, surface.x, surface.y, surface.width, surface.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, atlas.rgba.data());
        }
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}
