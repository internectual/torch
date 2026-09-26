#pragma once
// Client-side ShapeBaseImage state machine (ShapeBase::updateImageState /
// setImageState). The server sends only condition flags and a fire count;
// every client runs the datablock's state machine to pick the image's
// sequences, spin and sounds.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace WeaponImage {

// ShapeBaseImageData::StateData, as networked. Transitions are 0-based state
// slots, -1 for none.
struct StateData {
    bool valid = false;
    int transitionOnNotLoaded = -1, transitionOnLoaded = -1;
    int transitionOnNoAmmo = -1, transitionOnAmmo = -1;
    int transitionOnNoTarget = -1, transitionOnTarget = -1;
    int transitionOnNotWet = -1, transitionOnWet = -1;
    int transitionOnTriggerUp = -1, transitionOnTriggerDown = -1;
    int transitionOnTimeout = -1;
    float timeoutValue = 0.0f;
    bool waitForTimeout = false;
    bool fire = false;
    bool scaleAnimation = false;
    bool direction = true;      // false plays the sequence in reverse
    int spin = 0;               // 0 ignore, 1 stop, 2 up, 3 full... see Spin
    int sequence = -1;          // image shape sequence index
    int sequenceVis = -1;
    bool flashSequence = false;
    int sound = -1;             // AudioProfile datablock id
};

enum Spin { SpinIgnore = 0, SpinStop = 1, SpinUp = 2, SpinDown = 3, SpinFull = 4 };

struct Flags {
    bool triggerDown = false, loaded = false, ammo = false, wet = false, target = false;
    int fireCount = 0;
};

struct Output {
    int stateIndex = -1;
    int sequence = -1;
    bool flashSequence = false;
    int sequenceVis = -1;
    bool isFiring = false;
    float spinTimeScale = 0.0f;
    bool reverse = false;
    bool scaleAnimation = false;
    float timeoutValue = 0.0f;
    bool transitioned = false;
    bool entered = false;
    std::vector<int> sounds;
};

class StateMachine {
public:
    StateMachine() = default;
    explicit StateMachine(std::vector<StateData> states) : states_(std::move(states)) {
        for (size_t i = 0; i < states_.size(); ++i)
            if (states_[i].valid && states_[i].fire) { fireState_ = (int)i; break; }
        if (!states_.empty()) delay_ = states_[0].timeoutValue;
    }

    bool empty() const { return states_.empty(); }
    int stateIndex() const { return current_; }

    Output snapshot(bool withSound) const {
        Output out = output(false, true);
        const int sound = state(current_).sound;
        if (withSound && sound >= 0) out.sounds.push_back(sound);
        return out;
    }

    // Advance dt seconds. A changed fire count (or forceFire) forces the
    // fire state, as ShapeBase::unpackUpdate does for ghosts.
    Output tick(float dt, const Flags& flags, bool forceFire = false) {
        if (states_.empty()) return {};
        const bool fireCountChanged = lastFireCount_ >= 0 && flags.fireCount != lastFireCount_;
        lastFireCount_ = flags.fireCount;
        std::vector<int> sounds;
        delay_ -= dt;
        bool transitioned = false, entered = false;
        bool forced = (forceFire || fireCountChanged) && fireState_ >= 0;
        int next = forced ? fireState_ : tickTransitions(flags);
        for (int guard = 0; next >= 0 && guard < 32; ++guard) {
            if (!valid(next)) break;
            if (!forced) {
                // Ghosts wait for the server's fire notification.
                if (next == fireState_ && next != current_) break;
                if (next == current_) {
                    transitioned = true;
                    delay_ = state(next).timeoutValue;
                    break;
                }
            }
            transitioned = entered = true;
            forced = false;
            const int lastSpin = state(current_).spin;
            const float lastDelay = delay_;
            current_ = next;
            const float timeout = state(next).timeoutValue;
            delay_ = timeout;
            if (state(next).sound >= 0) sounds.push_back(state(next).sound);
            switch (state(next).spin) {
            case SpinStop: spinTimeScale_ = 0.0f; break;
            case SpinFull: spinTimeScale_ = 1.0f; break;
            case SpinUp:
                if (lastSpin == SpinDown && timeout > 0.0f) delay_ *= 1.0f - lastDelay / timeout;
                break;
            case SpinDown:
                if (lastSpin == SpinUp && timeout > 0.0f) delay_ *= 1.0f - lastDelay / timeout;
                break;
            default: break;
            }
            next = conditionTransition(state(current_), flags);
        }
        const StateData& s = state(current_);
        const float timeout = s.timeoutValue;
        switch (s.spin) {
        case SpinStop: spinTimeScale_ = 0.0f; break;
        case SpinUp: spinTimeScale_ = timeout > 0.0f ? std::max(0.0f, 1.0f - delay_ / timeout) : 1.0f; break;
        case SpinFull: spinTimeScale_ = 1.0f; break;
        case SpinDown: spinTimeScale_ = timeout > 0.0f ? std::max(0.0f, delay_ / timeout) : 0.0f; break;
        default: break;
        }
        Output out = output(transitioned, entered);
        out.sounds = std::move(sounds);
        return out;
    }

private:
    std::vector<StateData> states_;
    int current_ = 0;
    float delay_ = 0.0f;
    int lastFireCount_ = -1;
    float spinTimeScale_ = 0.0f;
    int fireState_ = -1;

    bool valid(int i) const { return i >= 0 && i < (int)states_.size() && states_[i].valid; }
    const StateData& state(int i) const {
        static const StateData none;
        return i >= 0 && i < (int)states_.size() ? states_[i] : none;
    }
    Output output(bool transitioned, bool entered) const {
        const StateData& s = state(current_);
        Output out;
        out.stateIndex = current_;
        out.sequence = s.sequence;
        out.flashSequence = s.flashSequence && s.sequenceVis >= 0;
        out.sequenceVis = s.sequenceVis;
        out.isFiring = s.fire;
        out.spinTimeScale = spinTimeScale_;
        out.reverse = !s.direction;
        out.scaleAnimation = s.scaleAnimation;
        out.timeoutValue = s.timeoutValue;
        out.transitioned = transitioned;
        out.entered = entered;
        return out;
    }
    // updateImageState: conditions (and the timeout) once the wait is over.
    int tickTransitions(const Flags& flags) const {
        const StateData& s = state(current_);
        const bool timedOut = delay_ <= 0.0f;
        if (!timedOut && s.waitForTimeout) return -1;
        const int condition = conditionTransition(s, flags);
        if (condition >= 0) return condition;
        if (timedOut && s.transitionOnTimeout >= 0) return s.transitionOnTimeout;
        return -1;
    }
    // Engine priority: loaded, ammo, target, wet, trigger.
    static int conditionTransition(const StateData& s, const Flags& f) {
        const int loaded = f.loaded ? s.transitionOnLoaded : s.transitionOnNotLoaded;
        if (loaded >= 0) return loaded;
        const int ammo = f.ammo ? s.transitionOnAmmo : s.transitionOnNoAmmo;
        if (ammo >= 0) return ammo;
        const int target = f.target ? s.transitionOnTarget : s.transitionOnNoTarget;
        if (target >= 0) return target;
        const int wet = f.wet ? s.transitionOnWet : s.transitionOnNotWet;
        if (wet >= 0) return wet;
        const int trigger = f.triggerDown ? s.transitionOnTriggerDown : s.transitionOnTriggerUp;
        return trigger;
    }
};

// A thread started by a state entry.
struct Thread {
    int sequence = -1;
    float startedAt = 0.0f;
    bool reverse = false;
    float timeout = -1.0f;     // scale the sequence to this many seconds
    int scaleSequence = -1;    // flash: use this sequence's duration to scale
    float position = -1.0f;    // fixed position (random flash pose)
    bool reset = false;        // a later state reset a cyclic thread
    bool valid() const { return sequence >= 0; }
};

// Normalized position of a thread at `now`.
inline float threadPosition(const Thread& t, float now, float duration, bool cyclic,
                            float scaleDuration = -1.0f) {
    if (t.position >= 0.0f) return t.position;
    if (t.reset && cyclic) return 0.0f;
    if (scaleDuration < 0.0f) scaleDuration = duration;
    const float period = t.timeout > 0.0f && scaleDuration > 0.0f
        ? duration * t.timeout / scaleDuration : duration;
    if (!(period > 0.0f)) return 0.0f;
    const float elapsed = std::max(0.0f, now - t.startedAt) / period;
    const float p = t.reverse ? 1.0f - elapsed : elapsed;
    if (cyclic) { const float f = p - (float)(int64_t)p; return f < 0.0f ? f + 1.0f : f; }
    return std::clamp(p, 0.0f, 1.0f);
}

// Per-image animation state kept on the image slot.
class Animation {
public:
    Animation() = default;
    Animation(std::vector<StateData> states, float now, uint32_t seed)
        : machine_(std::move(states)), seed_(seed ? seed : 0x9e3779b9u), time_(now), mountedAt_(now) {
        state_ = machine_.snapshot(true);
        pendingSounds_ = state_.sounds;
        enter(state_, now);
    }

    bool valid() const { return !machine_.empty(); }
    float mountedAt() const { return mountedAt_; }

    void advance(float now, const Flags& flags, bool forceFire = false) {
        if (machine_.empty()) return;
        const float dt = std::max(0.0f, now - time_);
        spinTime_ += dt * state_.spinTimeScale;
        time_ = now;
        Output out = machine_.tick(dt, flags, forceFire);
        pendingSounds_.insert(pendingSounds_.end(), out.sounds.begin(), out.sounds.end());
        if (out.entered || (out.transitioned && out.flashSequence)) {
            enter(out, now);
        } else if (out.spinTimeScale != state_.spinTimeScale) {
            spinTimeAtChange_ = spinTime_;
            spinChangedAt_ = now;
            state_ = out;
        } else {
            state_ = out;
        }
    }

    const Output& state() const { return state_; }
    const Thread& anim() const { return anim_; }
    const Thread& flash() const { return flash_; }
    // Spin thread time in seconds at `now`.
    float spinTime(float now) const {
        return spinTimeAtChange_ + std::max(0.0f, now - spinChangedAt_) * state_.spinTimeScale;
    }
    // AudioProfile ids entered since the last call.
    std::vector<int> takeSounds() { std::vector<int> s; s.swap(pendingSounds_); return s; }

private:
    StateMachine machine_;
    Output state_;
    Thread anim_, flash_;
    uint32_t seed_ = 1;
    float time_ = 0.0f, mountedAt_ = 0.0f;
    float spinTime_ = 0.0f, spinTimeAtChange_ = 0.0f, spinChangedAt_ = 0.0f;
    std::vector<int> pendingSounds_;

    float random() {
        // Deterministic per image, so replays and seeks match.
        seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
        return (float)(seed_ & 0xffffff) / 16777216.0f;
    }

    void enter(const Output& out, float now) {
        if (out.entered) {
            if (anim_.valid()) anim_.reset = true;
            if (flash_.valid()) flash_.position = 0.0f;
        }
        if (out.sequence >= 0 && (out.entered || out.flashSequence)) {
            anim_ = {};
            anim_.sequence = out.sequence;
            anim_.startedAt = now;
            anim_.reverse = out.reverse;
            anim_.timeout = out.scaleAnimation && out.timeoutValue > 0.0f ? out.timeoutValue : -1.0f;
            if (out.flashSequence) {
                anim_.position = random();
                if (out.sequenceVis >= 0) {
                    flash_ = {};
                    flash_.sequence = out.sequenceVis;
                    flash_.startedAt = now;
                    flash_.timeout = anim_.timeout;
                    flash_.scaleSequence = anim_.sequence;
                }
            }
        }
        state_ = out;
        spinTimeAtChange_ = spinTime_;
        spinChangedAt_ = now;
    }
};

} // namespace WeaponImage
