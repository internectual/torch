#include "render/dif_lighting.h"
#include <cassert>
#include <cmath>

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static DIFLightState state(uint8_t r, uint8_t g, uint8_t b, uint32_t activeTime,
                           uint32_t dataIndex = 0, uint16_t dataCount = 0) {
    DIFLightState s;
    s.color[0] = r; s.color[1] = g; s.color[2] = b;
    s.activeTime = activeTime;
    s.dataIndex = dataIndex;
    s.dataCount = dataCount;
    return s;
}

int main() {
    // A looping ambient light: colours interpolate towards the next state,
    // the last state towards the first, and time wraps at the duration.
    const std::vector<DIFLightState> states = {state(0, 0, 0, 0), state(200, 100, 50, 500)};
    DIFAnimatedLight loop{0, 0, 2, DIFLight_Ambient | DIFLight_Loop, 1000};
    DIFLightSample sample;
    sampleDIFLight(loop, states, 250.0, 0, sample);
    assert(sample.state == 0 && sample.color[0] == 100 && sample.color[1] == 50 && sample.color[2] == 25);
    sampleDIFLight(loop, states, 750.0, 0, sample);
    assert(sample.state == 1 && sample.color[0] == 100);
    sampleDIFLight(loop, states, 1250.0, 0, sample);
    assert(sample.state == 0 && sample.color[0] == 100);

    // Without Ambient or Alarm a light (a triggered one) holds its first state.
    DIFAnimatedLight triggered{0, 0, 2, DIFLight_Loop, 1000};
    sampleDIFLight(triggered, states, 750.0, 0, sample);
    assert(sample.state == 0 && sample.color[0] == 0);

    // A one-shot light stops on its last state.
    DIFAnimatedLight once{0, 0, 2, DIFLight_Ambient, 1000};
    sampleDIFLight(once, states, 5000.0, 0, sample);
    assert(sample.state == 1 && sample.color[0] == 200);

    // Flicker: the first period shows the first state; later periods jump to
    // a repeatable random time with no interpolation.
    DIFAnimatedLight flicker{0, 0, 2, DIFLight_Ambient | DIFLight_Flicker, 1000};
    sampleDIFLight(flicker, states, 100.0, 7, sample);
    assert(sample.state == 0 && sample.color[0] == 0);
    DIFLightSample a, b;
    sampleDIFLight(flicker, states, 1320.0, 7, a);
    sampleDIFLight(flicker, states, 1320.0, 7, b);
    assert(a.state == b.state && a.color[0] == b.color[0]);
    assert(a.color[0] == (a.state ? 200 : 0));

    // Plans: surface 0 lit by a normal light into lightmap 1, surface 1 by an
    // alarm light into alarm lightmap 2. The last state data for a slot wins.
    const std::vector<DIFAnimatedLight> lights = {
        {0, 0, 1, DIFLight_Ambient, 0},
        {0, 1, 1, DIFLight_Alarm | DIFLight_Loop, 1000},
    };
    const std::vector<DIFLightState> planStates = {state(255, 0, 0, 0, 0, 2), state(0, 0, 255, 0, 2, 1)};
    const std::vector<DIFLightStateData> stateData = {
        {0, 0, 0}, {0, 4, 0}, {1, 8, 1},
    };
    std::vector<DIFLitSurface> surfaces(2);
    surfaces[0] = {1, 0, 1, 2, 2, 2};
    surfaces[1] = {1, 1, 0, 0, 2, 1};
    const char* error = nullptr;
    assert(validateDIFLighting(lights, planStates, stateData, 12, surfaces, error));
    assert(!validateDIFLighting(lights, planStates, stateData, 9, surfaces, error));
    auto plans = buildDIFLightingPlans(lights, planStates, stateData, surfaces, {1, 1}, {1, 2}, true);
    assert(plans.size() == 2);
    const DIFLightingPlan& normal = plans.at(difLightingPlanKey(1, false));
    assert(normal.surfaces.size() == 1 && normal.lights == std::vector<uint32_t>{0});
    assert(normal.surfaces[0].x == 1 && normal.surfaces[0].y == 2 && normal.surfaces[0].width == 2);
    assert(normal.surfaces[0].slots[0].maps[0] == 4);
    const DIFLightingPlan& alarm = plans.at(difLightingPlanKey(2, true));
    assert(alarm.surfaces.size() == 1 && alarm.lights == std::vector<uint32_t>{1});
    // No alarm state: no alarm plans.
    assert(buildDIFLightingPlans(lights, planStates, stateData, surfaces, {1, 1}, {1, 2}, false).size() == 1);

    // Relight: the base patch is restored, then colour x intensity is added
    // with rounding and saturates at 255.
    const int width = 4, height = 4;
    std::vector<uint8_t> base(width * height * 4, 100), out(base.size(), 0);
    std::vector<uint8_t> buffer(12, 0);
    buffer[4] = 128; buffer[5] = 255; buffer[6] = 0; buffer[7] = 64;
    std::vector<DIFLightSample> samples(2);
    samples[0].state = 0;
    samples[0].color[0] = 200; samples[0].color[1] = 100;
    for (int pass = 0; pass < 2; ++pass) {
        compositeDIFLightmap(normal, samples, buffer, base.data(), out.data(), width);
        const uint8_t* p = &out[((2 * width) + 1) * 4];
        assert(p[0] == 100 + ((200 * 128 + 128) >> 8) && p[1] == 100 + ((100 * 128 + 128) >> 8) && p[2] == 100);
        assert(p[4] == 255);                       // 100 + 199 saturates
        assert(out[((3 * width) + 1) * 4] == 100); // intensity 0
        assert(out[0] == 0);                       // outside the patch
    }

    // Lighting clocks: the normal clock pauses while the alarm runs.
    DIFLightingClock clock;
    clock.startSec = 10.0;
    clock.alarmChangedAtSec = 10.0;
    assert(near(clock.lightingTime(15.0), 5.0));
    assert(clock.setAlarm(true, 15.0) && !clock.setAlarm(true, 16.0));
    assert(near(clock.lightingTime(18.0), 3.0));
    assert(clock.setAlarm(false, 20.0));
    assert(near(clock.lightingTime(25.0), 10.0));
    assert(clock.setAlarm(true, 30.0));
    assert(near(clock.lightingTime(31.0), 6.0));
    return 0;
}
