#pragma once
#include "core/math.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct AudioConfig {
    bool enabled = true;
    float masterVolume = 1.0f;
    float sfxVolume = 1.0f;
    float musicVolume = 0.5f;
    int32_t maxSources = 32;
};

struct SoundBuffer {
    uint32_t buffer{};
    bool loaded = false;
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
    bool positional = false;

    void play(SoundBuffer* buffer);
    void stop();
    void pause();
    void resume();
    void setVolume(float vol);
    void setPitch(float p);
    void setPosition(const Point3F& pos);
    void setLooping(bool loop);
    bool isPlaying() const;
    void destroy();
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    bool init();
    void shutdown();

    void update(const Point3F& listenerPos, const Point3F& listenerVel,
                const Point3F& listenerForward, const Point3F& listenerUp);

    SoundBuffer* loadSound(const char* path);
    SoundSource* createSource();
    void releaseSource(SoundSource* source);

    AudioConfig& config() { return cfg; }
    bool isInitialized() const { return initialized; }

    void stopAll();
    void pauseAll();
    void resumeAll();

private:
    struct Impl;
    Impl* impl;
    AudioConfig cfg;
    bool initialized = false;
};
