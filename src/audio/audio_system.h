#pragma once
#include "core/math.h"
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <cstddef>

struct AudioConfig {
    bool enabled = true;
    float masterVolume = 1.0f;
    float sfxVolume = 1.0f;
    float musicVolume = 0.5f;
    int32_t maxSources = 32;
};

namespace AudioSourcePolicy {
struct Candidate {
    int priority = 0;
    bool persistent = false;
    bool paused = false;
    uint64_t serial = 0;
};

// Return the oldest lower-priority source that may be reclaimed, or -1 when
// the request must be rejected. This is deliberately GL-free for deterministic
// allocation tests and keeps protected script sources out of the steal pool.
inline int selectSteal(const std::vector<Candidate>& candidates, int priority) {
    int selected = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.persistent || candidate.paused || candidate.priority >= priority)
            continue;
        if (selected < 0 || candidate.priority < candidates[selected].priority ||
            (candidate.priority == candidates[selected].priority &&
             candidate.serial < candidates[selected].serial))
            selected = static_cast<int>(i);
    }
    return selected;
}

inline uint32_t nextLoopGapMs(uint32_t& state, int32_t minimum, int32_t maximum) {
    const uint32_t lo = static_cast<uint32_t>(std::max(0, minimum));
    const uint32_t hi = static_cast<uint32_t>(std::max(static_cast<int32_t>(lo), maximum));
    state = state * 1664525u + 1013904223u;
    return lo + (hi == lo ? 0u : state % (hi - lo + 1u));
}
}

struct SoundBuffer {
    uint32_t buffer{};
    bool loaded = false;
    uint32_t durationMs = 0;
    bool load(const uint8_t* data, size_t size);
    bool loadWav(const uint8_t* data, size_t size);
    bool loadOgg(const uint8_t* data, size_t size);
    void destroy();
};

struct SoundSource {
    uint32_t source{};
    bool playing = false;
    bool paused = false;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool looping = false;
    Point3F position{};
    Point3F velocity{};
    bool positional = false;
    bool relative = true;
    bool persistent = false; // Script-created sources remain valid after playback stops.
    int priority = 0;
    uint64_t serial = 0;
    bool underwater = false;
    // 0 is unobstructed and 1 is fully occluded. The gain reduction is kept
    // independent of EFX so occlusion remains safe on fallback OpenAL devices.
    float occlusion = 0.0f;
    float referenceDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;
    float offsetSeconds = 0.0f;
    uint32_t auxiliarySend = 0;
    int auxiliarySendIndex = 0;
    uint32_t auxiliaryFilter = 0;
    uint32_t environmentSend = 0;
    SoundBuffer* scheduledBuffer = nullptr;
    bool loopScheduleActive = false;
    int32_t loopRepeatsRemaining = 0;
    int32_t loopGapMinMs = 0;
    int32_t loopGapMaxMs = 0;
    uint32_t loopRandomState = 1;
    double scheduledElapsed = 0.0;
    double scheduledGap = 0.0;

    static constexpr float clampRolloff(float factor) {
        return factor < 0.0f ? 0.0f : factor;
    }
    static constexpr float clampPitch(float value) {
        return value < 0.01f ? 0.01f : value;
    }

    void play(SoundBuffer* buffer);
    void stop();
    void pause();
    void resume();
    void setVolume(float vol);
    void setUnderwater(bool underwater, uint32_t filter = 0);
    void setOcclusion(float amount);
    void setPitch(float p);
    void setPosition(const Point3F& pos);
    void setVelocity(const Point3F& velocity);
    void setRelative(bool relative);
    void setAuxiliarySend(uint32_t slot, int sendIndex = 0, uint32_t filter = 0);
    void setEnvironmentSend(uint32_t slot, bool enabled);
    void setDistance(float reference, float maxDistance);
    void setRolloff(float factor);
    void setOffsetSeconds(float seconds);
    static constexpr float clampOffsetSeconds(float seconds, uint32_t durationMs) {
        if (!std::isfinite(seconds) || seconds < 0.0f) return 0.0f;
        const float duration = static_cast<float>(durationMs) / 1000.0f;
        return duration > 0.0f ? std::min(seconds, duration) : seconds;
    }
    void setLooping(bool loop);
    void setLoopSchedule(int32_t loopCount, int32_t minGapMs, int32_t maxGapMs,
                         uint32_t deterministicSeed = 1);
    void advance(float seconds);
    bool isPlaying() const;
    void destroy();
};

struct AudioEnvironmentState {
    bool enabled = false;
    std::unordered_map<std::string, int> ints;
    std::unordered_map<std::string, float> floats;

    static constexpr float clamp(float value) {
        return value < -100000.0f ? -100000.0f :
               value > 100000.0f ? 100000.0f : value;
    }
    bool setInt(const std::string& name, int value) {
        if (name.empty()) return false;
        ints[name] = value;
        return true;
    }
    bool setFloat(const std::string& name, float value) {
        if (name.empty() || !std::isfinite(value)) return false;
        floats[name] = clamp(value);
        return true;
    }
    void clear() { enabled = false; ints.clear(); floats.clear(); }
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    bool init();
    void shutdown();

    void update(const Point3F& listenerPos, const Point3F& listenerVel,
                const Point3F& listenerForward, const Point3F& listenerUp,
                bool underwater = false);
    void setListenerPosition(const Point3F& position);
    void setListenerVelocity(const Point3F& velocity);
    void setListenerOrientation(const Point3F& forward, const Point3F& up);
    void setUnderwater(bool underwater);
    void setListenerGain(float gain);
    float listenerGain() const { return listenerGain_; }
    bool isUnderwater() const { return underwater_; }
    void setEnvironmental(bool enabled);
    void clearEnvironmentState();
    bool environmentalEnabled() const { return environment_.enabled; }
    bool environmentalSupported() const;
    bool setEnvironmenti(const std::string& name, int value);
    bool setEnvironmentf(const std::string& name, float value);
    int environmenti(const std::string& name) const;
    float environmentf(const std::string& name) const;

    // These values are deliberately simple: V12 provides no authored reverb
    // data for this path, so underwater sound uses filtering and attenuation.
    static constexpr float underwaterSourceGain() { return 0.72f; }
    static constexpr float underwaterListenerGain() { return 0.65f; }
    static constexpr float underwaterHighFrequencyGain() { return 0.18f; }
    static constexpr uint32_t selectWetEffect(bool underwater, uint32_t dry, uint32_t wet) {
        return underwater && wet ? wet : dry;
    }
    static constexpr float occlusionGain(float amount) {
        return 1.0f - (amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount) * 0.75f;
    }
    static constexpr float clampEnvironmentValue(float value) { return AudioEnvironmentState::clamp(value); }

    SoundBuffer* loadSound(const char* path);
    SoundSource* createSource(bool persistent = false, int priority = 0);
    void releaseSource(SoundSource* source);
    bool isSourceAlive(const SoundSource* source) const;

    AudioConfig& config() { return cfg; }
    bool isInitialized() const { return initialized; }

    void stopAll();
    void pauseAll();
    void resumeAll();
    void setPlaybackRate(float rate);
    void advance(float seconds);
    float playbackRate() const { return playbackRate_; }

private:
    struct Impl;
    Impl* impl;
    AudioConfig cfg;
    bool initialized = false;
    float playbackRate_ = 1.0f;
    bool underwater_ = false;
    float listenerGain_ = 1.0f;
    AudioEnvironmentState environment_;

    void applyEnvironment();
};
