#pragma once
#include "render/renderer.h"
#include <cstdint>
#include <map>
#include <vector>

// One interior instance's lighting mode and animated lightmaps (t2-mapper
// DIFLighting). Geometry, the base lightmaps and the relight plans stay
// shared on the DTSShape; an instance owns copies of the lightmaps its
// animated lights rewrite.
class DIFLightingInstance {
public:
    explicit DIFLightingInstance(double startSec);
    ~DIFLightingInstance();
    DIFLightingInstance(const DIFLightingInstance&) = delete;
    DIFLightingInstance& operator=(const DIFLightingInstance&) = delete;

    // The client side of InteriorInstance::setAlarmMode: ignored for an
    // interior without an alarm state. True when the mode changed.
    bool setAlarm(const DTSShape& shape, bool alarm, double nowSec);
    bool alarm() const { return clock.alarm; }
    // Relights the current mode's animated lightmaps from the lighting clock,
    // at most once per 66 ms bucket. Call only before a visible draw.
    void prepare(const DTSShape& shape, double nowSec);
    // The animated copy of `lightmap` in the current mode, 0 if it has none.
    uint32_t animatedLightmap(int lightmap) const;

private:
    struct Atlas {
        Texture texture;
        std::vector<uint8_t> rgba;
        int64_t bucket = INT64_MIN;
        std::vector<uint32_t> versions; // per plan light, at the last relight
    };
    DIFLightingClock clock;
    std::vector<DIFLightSample> samples;  // per light
    std::vector<int64_t> sampleBuckets;   // per light
    std::vector<uint32_t> sampleVersions; // per light, bumped on change
    std::map<int, Atlas> atlases;         // by difLightingPlanKey
};
