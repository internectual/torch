#include "game/weapon_image_state.h"
#include <cassert>
#include <cmath>

using namespace WeaponImage;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static std::vector<StateData> discStates() {
    std::vector<StateData> s(31);
    s[0].valid = true; s[0].timeoutValue = 0.5f; s[0].transitionOnTimeout = 1;
    s[0].sequence = 4; s[0].sound = 100; // Activate
    s[1].valid = true; s[1].transitionOnTriggerDown = 2; s[1].sequence = 5; // Ready
    s[2].valid = true; s[2].fire = true; s[2].timeoutValue = 0.2f; s[2].waitForTimeout = true;
    s[2].transitionOnTimeout = 3; s[2].sequence = 6; s[2].sound = 101; // Fire
    s[3].valid = true; s[3].timeoutValue = 0.5f; s[3].waitForTimeout = true;
    s[3].transitionOnTimeout = 1; s[3].sequence = 7; s[3].scaleAnimation = true; // Reload
    return s;
}

int main() {
    StateMachine m(discStates());
    Flags f;
    Output o = m.tick(0.1f, f);
    assert(o.stateIndex == 0 && !o.entered);
    o = m.tick(0.5f, f);
    assert(o.stateIndex == 1 && o.entered);
    // A ghost does not enter the fire state from the trigger alone.
    f.triggerDown = true;
    o = m.tick(0.032f, f);
    assert(o.stateIndex == 1);
    // A changed fire count forces the fire state, playing its sound.
    f.fireCount = 1;
    o = m.tick(0.032f, f);
    assert(o.stateIndex == 2 && o.entered && o.isFiring);
    assert(o.sounds.size() == 1 && o.sounds[0] == 101);
    // Waits for the timeout, then reloads and returns to ready.
    o = m.tick(0.1f, f);
    assert(o.stateIndex == 2);
    o = m.tick(0.15f, f);
    assert(o.stateIndex == 3 && o.scaleAnimation && near(o.timeoutValue, 0.5f));
    f.triggerDown = false;
    o = m.tick(0.6f, f);
    assert(o.stateIndex == 1);

    // Spin up ramps with the remaining timeout.
    std::vector<StateData> spin(31);
    spin[0].valid = true; spin[0].spin = SpinUp; spin[0].timeoutValue = 1.0f;
    StateMachine s(spin);
    o = s.tick(0.25f, {});
    assert(near(o.spinTimeScale, 0.25f));

    // Animation keeps the entered sequence and the activation sound.
    Animation a(discStates(), 10.0f, 7);
    assert(a.state().stateIndex == 0 && a.anim().sequence == 4);
    auto sounds = a.takeSounds();
    assert(sounds.size() == 1 && sounds[0] == 100);
    a.advance(10.6f, {});
    assert(a.anim().sequence == 5 && near(a.anim().startedAt, 10.6f));
    // Reload scales its sequence to the 0.5 s timeout.
    Flags fire; fire.fireCount = 0;
    a.advance(10.7f, fire);
    fire.fireCount = 1;
    a.advance(10.8f, fire);
    a.advance(11.1f, fire);
    assert(a.anim().sequence == 7 && near(a.anim().timeout, 0.5f));
    assert(near(threadPosition(a.anim(), 11.35f, 2.0f, false), 0.5f));

    // Thread position: cyclic wraps, reverse counts down.
    Thread t; t.sequence = 1; t.startedAt = 0.0f;
    assert(near(threadPosition(t, 1.5f, 1.0f, true), 0.5f));
    t.reverse = true;
    assert(near(threadPosition(t, 0.25f, 1.0f, false), 0.75f));
    return 0;
}
