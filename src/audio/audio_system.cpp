#include "audio/audio_system.h"
#include "core/console.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>
#include <vorbis/vorbisfile.h>
#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace {
Point3F safeUnit(Point3F value, Point3F fallback) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!std::isfinite(length) || length < 0.0001f) return fallback;
    return {value.x / length, value.y / length, value.z / length};
}
}

struct AudioSystem::Impl {
    using GenFiltersFn = void (*)(ALsizei, ALuint*);
    using DeleteFiltersFn = void (*)(ALsizei, const ALuint*);
    using FilteriFn = void (*)(ALuint, ALenum, ALint);
    using FilterfFn = void (*)(ALuint, ALenum, ALfloat);
    using GenEffectsFn = void (*)(ALsizei, ALuint*);
    using DeleteEffectsFn = void (*)(ALsizei, const ALuint*);
    using EffectiFn = void (*)(ALuint, ALenum, ALint);
    using EffectfFn = void (*)(ALuint, ALenum, ALfloat);
    using GenSlotsFn = void (*)(ALsizei, ALuint*);
    using DeleteSlotsFn = void (*)(ALsizei, const ALuint*);
    using SlotiFn = void (*)(ALuint, ALenum, ALint);
    using SlotfFn = void (*)(ALuint, ALenum, ALfloat);
    ALCdevice* device{};
    ALCcontext* context{};
    std::unordered_map<std::string, SoundBuffer*> buffers;
    std::vector<SoundSource*> sources;
    uint64_t nextSourceSerial = 1;
    ALuint underwaterFilter{};
    bool hasEfx = false;
    GenFiltersFn genFilters{};
    DeleteFiltersFn deleteFilters{};
    FilteriFn filteri{};
    FilterfFn filterf{};
    ALuint reverbEffect{};
    ALuint reverbSlot{};
    GenEffectsFn genEffects{};
    DeleteEffectsFn deleteEffects{};
    EffectiFn effecti{};
    EffectfFn effectf{};
    GenSlotsFn genSlots{};
    DeleteSlotsFn deleteSlots{};
    SlotiFn sloti{};
    SlotfFn slotf{};
};

AudioSystem::AudioSystem() : impl(new Impl) {}
AudioSystem::~AudioSystem() {
    if (initialized || !impl->sources.empty() || !impl->buffers.empty())
        shutdown();
    delete impl;
}

bool AudioSystem::init() {
    if (initialized) return true;
    impl->device = alcOpenDevice(nullptr);
    if (!impl->device) {
        Console::instance().printf(LogLevel::Warn, "OpenAL: cannot open device");
        return false;
    }

    impl->context = alcCreateContext(impl->device, nullptr);
    if (!impl->context) {
        alcCloseDevice(impl->device);
        Console::instance().printf(LogLevel::Warn, "OpenAL: cannot create context");
        return false;
    }

    alcMakeContextCurrent(impl->context);

    Console::instance().printf(LogLevel::Info, "Audio: OpenAL initialized (%s)",
        alcGetString(impl->device, ALC_DEVICE_SPECIFIER));

    // Set listener defaults
    alListener3f(AL_POSITION, 0, 0, 0);
    alListener3f(AL_VELOCITY, 0, 0, 0);

    impl->hasEfx = alIsExtensionPresent("AL_EXT_EFX") == AL_TRUE;
    if (impl->hasEfx) {
        impl->genFilters = reinterpret_cast<Impl::GenFiltersFn>(alGetProcAddress("alGenFilters"));
        impl->deleteFilters = reinterpret_cast<Impl::DeleteFiltersFn>(alGetProcAddress("alDeleteFilters"));
        impl->filteri = reinterpret_cast<Impl::FilteriFn>(alGetProcAddress("alFilteri"));
        impl->filterf = reinterpret_cast<Impl::FilterfFn>(alGetProcAddress("alFilterf"));
        impl->genEffects = reinterpret_cast<Impl::GenEffectsFn>(alGetProcAddress("alGenEffects"));
        impl->deleteEffects = reinterpret_cast<Impl::DeleteEffectsFn>(alGetProcAddress("alDeleteEffects"));
        impl->effecti = reinterpret_cast<Impl::EffectiFn>(alGetProcAddress("alEffecti"));
        impl->effectf = reinterpret_cast<Impl::EffectfFn>(alGetProcAddress("alEffectf"));
        impl->genSlots = reinterpret_cast<Impl::GenSlotsFn>(alGetProcAddress("alGenAuxiliaryEffectSlots"));
        impl->deleteSlots = reinterpret_cast<Impl::DeleteSlotsFn>(alGetProcAddress("alDeleteAuxiliaryEffectSlots"));
        impl->sloti = reinterpret_cast<Impl::SlotiFn>(alGetProcAddress("alAuxiliaryEffectSloti"));
        impl->slotf = reinterpret_cast<Impl::SlotfFn>(alGetProcAddress("alAuxiliaryEffectSlotf"));
        impl->hasEfx = impl->genFilters && impl->deleteFilters && impl->filteri && impl->filterf &&
                       impl->genEffects && impl->deleteEffects && impl->effecti && impl->effectf &&
                       impl->genSlots && impl->deleteSlots && impl->sloti && impl->slotf;
        if (impl->hasEfx) impl->genFilters(1, &impl->underwaterFilter);
        if (alGetError() != AL_NO_ERROR || !impl->underwaterFilter) {
            impl->underwaterFilter = 0;
            impl->hasEfx = false;
        } else {
            impl->filteri(impl->underwaterFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
            impl->filterf(impl->underwaterFilter, AL_LOWPASS_GAIN, 1.0f);
            impl->filterf(impl->underwaterFilter, AL_LOWPASS_GAINHF,
                          AudioSystem::underwaterHighFrequencyGain());
            impl->genEffects(1, &impl->reverbEffect);
            impl->genSlots(1, &impl->reverbSlot);
            if (alGetError() != AL_NO_ERROR || !impl->reverbEffect || !impl->reverbSlot) {
                if (impl->reverbSlot) impl->deleteSlots(1, &impl->reverbSlot);
                if (impl->reverbEffect) impl->deleteEffects(1, &impl->reverbEffect);
                impl->reverbSlot = impl->reverbEffect = 0;
                impl->deleteFilters(1, &impl->underwaterFilter);
                impl->underwaterFilter = 0;
                impl->hasEfx = false;
            } else {
                impl->effecti(impl->reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
                impl->sloti(impl->reverbSlot, AL_EFFECTSLOT_EFFECT, (ALint)impl->reverbEffect);
                impl->slotf(impl->reverbSlot, AL_EFFECTSLOT_GAIN, 0.0f);
                if (alGetError() != AL_NO_ERROR) {
                    impl->deleteSlots(1, &impl->reverbSlot);
                    impl->deleteEffects(1, &impl->reverbEffect);
                    impl->deleteFilters(1, &impl->underwaterFilter);
                    impl->reverbSlot = impl->reverbEffect = impl->underwaterFilter = 0;
                    impl->hasEfx = false;
                }
            }
        }
    }

    initialized = true;
    return true;
}

void AudioSystem::shutdown() {
    for (auto* source : impl->sources) {
        if (!source) continue;
        source->stop();
        source->destroy();
        delete source;
    }
    impl->sources.clear();
    for (auto& [k, v] : impl->buffers) delete v;
    impl->buffers.clear();

    if (impl->underwaterFilter) {
        impl->deleteFilters(1, &impl->underwaterFilter);
        impl->underwaterFilter = 0;
    }
    if (impl->reverbSlot) impl->deleteSlots(1, &impl->reverbSlot);
    if (impl->reverbEffect) impl->deleteEffects(1, &impl->reverbEffect);
    impl->reverbSlot = impl->reverbEffect = 0;
    impl->hasEfx = false;

    if (impl->context) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(impl->context);
        impl->context = nullptr;
    }
    if (impl->device) {
        alcCloseDevice(impl->device);
        impl->device = nullptr;
    }
    initialized = false;
    playbackRate_ = 1.0f;
    underwater_ = false;
    listenerGain_ = 1.0f;
    environment_ = {};
}

void AudioSystem::update(const Point3F& listenerPos, const Point3F& listenerVel,
                         const Point3F& listenerForward, const Point3F& listenerUp,
                         bool underwater) {
    if (!initialized) return;
    setUnderwater(underwater);
    const Point3F forward = safeUnit(listenerForward, {0.0f, 0.0f, -1.0f});
    Point3F up = safeUnit(listenerUp, {0.0f, 1.0f, 0.0f});
    // OpenAL requires the orientation vectors to be non-zero and orthogonal.
    const float dot = forward.x * up.x + forward.y * up.y + forward.z * up.z;
    Point3F orthogonalUp{up.x - forward.x * dot, up.y - forward.y * dot,
                         up.z - forward.z * dot};
    if (std::fabs(orthogonalUp.x) + std::fabs(orthogonalUp.y) +
        std::fabs(orthogonalUp.z) < 0.0001f)
        orthogonalUp = std::fabs(forward.y) < 0.9f ? Point3F{0.0f, 1.0f, 0.0f}
                                                    : Point3F{1.0f, 0.0f, 0.0f};
    up = safeUnit(orthogonalUp, {0.0f, 1.0f, 0.0f});
    ALfloat listenerOri[6] = {forward.x, forward.y, forward.z,
                              up.x, up.y, up.z};
    alListener3f(AL_POSITION, listenerPos.x, listenerPos.y, listenerPos.z);
    alListener3f(AL_VELOCITY, listenerVel.x, listenerVel.y, listenerVel.z);
    alListenerfv(AL_ORIENTATION, listenerOri);
    applyEnvironment();

    // Clean up one-shot sources that have finished playing
    for (auto it = impl->sources.begin(); it != impl->sources.end(); ) {
        SoundSource* src = *it;
        if (src && !src->persistent && !src->looping && !src->paused && !src->isPlaying()) {
            src->destroy();
            delete src;
            it = impl->sources.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::advance(float seconds) {
    if (!initialized || !std::isfinite(seconds) || seconds <= 0.0f) return;
    for (auto* source : impl->sources)
        if (source) source->advance(seconds);
}

void AudioSystem::setListenerPosition(const Point3F& position) {
    if (!initialized || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    alListener3f(AL_POSITION, position.x, position.y, position.z);
}

void AudioSystem::setListenerVelocity(const Point3F& velocity) {
    if (!initialized || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
        !std::isfinite(velocity.z)) return;
    alListener3f(AL_VELOCITY, velocity.x, velocity.y, velocity.z);
}

void AudioSystem::setListenerOrientation(const Point3F& forward, const Point3F& up) {
    if (!initialized) return;
    const Point3F direction = safeUnit(forward, {0.0f, 0.0f, -1.0f});
    Point3F normalizedUp = safeUnit(up, {0.0f, 1.0f, 0.0f});
    const float dot = direction.x * normalizedUp.x + direction.y * normalizedUp.y + direction.z * normalizedUp.z;
    normalizedUp = safeUnit({normalizedUp.x - direction.x * dot,
                             normalizedUp.y - direction.y * dot,
                             normalizedUp.z - direction.z * dot},
                            std::fabs(direction.y) < 0.9f ? Point3F{0.0f, 1.0f, 0.0f}
                                                          : Point3F{1.0f, 0.0f, 0.0f});
    const ALfloat orientation[6] = {direction.x, direction.y, direction.z,
                                    normalizedUp.x, normalizedUp.y, normalizedUp.z};
    alListenerfv(AL_ORIENTATION, orientation);
}

void AudioSystem::setUnderwater(bool underwater) {
    if (!initialized || underwater_ == underwater) return;
    underwater_ = underwater;
    alListenerf(AL_GAIN, listenerGain_ * (underwater_ ? underwaterListenerGain() : 1.0f));
    for (auto* source : impl->sources) {
        if (source) source->setUnderwater(underwater_, impl->hasEfx ? impl->underwaterFilter : 0);
    }
}

void AudioSystem::setListenerGain(float gain) {
    listenerGain_ = std::clamp(gain, 0.0f, 1.0f);
    if (initialized)
        alListenerf(AL_GAIN, listenerGain_ * (underwater_ ? underwaterListenerGain() : 1.0f));
}

void AudioSystem::setEnvironmental(bool enabled) {
    environment_.enabled = enabled;
    applyEnvironment();
}

void AudioSystem::clearEnvironmentState() {
    environment_.clear();
    environment_.enabled = false;
    applyEnvironment();
}

bool AudioSystem::setEnvironmenti(const std::string& name, int value) {
    if (!environment_.setInt(name, value)) return false;
    applyEnvironment();
    return true;
}

bool AudioSystem::setEnvironmentf(const std::string& name, float value) {
    if (!environment_.setFloat(name, value)) return false;
    applyEnvironment();
    return true;
}

bool AudioSystem::environmentalSupported() const {
    return initialized && impl->hasEfx && impl->reverbEffect && impl->reverbSlot;
}

void AudioSystem::applyEnvironment() {
    if (!environmentalSupported()) return;
    auto value = [this](const char* a, const char* b, float fallback) {
        auto it = environment_.floats.find(a);
        if (it == environment_.floats.end() && b) it = environment_.floats.find(b);
        return it == environment_.floats.end() ? fallback : it->second;
    };
    auto bounded = [](float v, float lo, float hi) { return std::clamp(v, lo, hi); };
    auto set = [this](ALenum param, float v) { impl->effectf(impl->reverbEffect, param, v); };
    set(AL_REVERB_DENSITY, bounded(value("density", "AL_ENV_DENSITY", AL_REVERB_DEFAULT_DENSITY), 0.0f, 1.0f));
    set(AL_REVERB_DIFFUSION, bounded(value("diffusion", "AL_ENV_DIFFUSION", AL_REVERB_DEFAULT_DIFFUSION), 0.0f, 1.0f));
    set(AL_REVERB_GAIN, bounded(value("gain", "AL_ENV_ROOM", AL_REVERB_DEFAULT_GAIN), 0.0f, 1.0f));
    set(AL_REVERB_GAINHF, bounded(value("gainhf", "AL_ENV_HF", AL_REVERB_DEFAULT_GAINHF), 0.0f, 1.0f));
    set(AL_REVERB_DECAY_TIME, bounded(value("decaytime", "AL_ENV_DECAY_TIME", AL_REVERB_DEFAULT_DECAY_TIME), 0.1f, 20.0f));
    set(AL_REVERB_DECAY_HFRATIO, bounded(value("decayhfratio", "AL_ENV_DECAY_HF_RATIO", AL_REVERB_DEFAULT_DECAY_HFRATIO), 0.1f, 2.0f));
    set(AL_REVERB_REFLECTIONS_GAIN, bounded(value("reflectionsgain", "AL_ENV_REFLECTIONS", AL_REVERB_DEFAULT_REFLECTIONS_GAIN), 0.0f, 3.16f));
    set(AL_REVERB_REFLECTIONS_DELAY, bounded(value("reflectionsdelay", "AL_ENV_REFLECTIONS_DELAY", AL_REVERB_DEFAULT_REFLECTIONS_DELAY), 0.0f, 0.3f));
    set(AL_REVERB_LATE_REVERB_GAIN, bounded(value("latereverbgain", "AL_ENV_REVERB", AL_REVERB_DEFAULT_LATE_REVERB_GAIN), 0.0f, 10.0f));
    set(AL_REVERB_LATE_REVERB_DELAY, bounded(value("latereverbdelay", "AL_ENV_REVERB_DELAY", AL_REVERB_DEFAULT_LATE_REVERB_DELAY), 0.0f, 0.1f));
    set(AL_REVERB_AIR_ABSORPTION_GAINHF, bounded(value("airabsorptiongainhf", nullptr, AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF), 0.892f, 1.0f));
    impl->slotf(impl->reverbSlot, AL_EFFECTSLOT_GAIN, environment_.enabled ? 1.0f : 0.0f);
    for (auto* source : impl->sources)
        if (source) source->setEnvironmentSend(impl->reverbSlot, environment_.enabled);
}

int AudioSystem::environmenti(const std::string& name) const {
    const auto it = environment_.ints.find(name);
    return it == environment_.ints.end() ? 0 : it->second;
}

float AudioSystem::environmentf(const std::string& name) const {
    const auto it = environment_.floats.find(name);
    return it == environment_.floats.end() ? 0.0f : it->second;
}

SoundBuffer* AudioSystem::loadSound(const char* path) {
    if (!initialized || !path || !*path) return nullptr;
    auto it = impl->buffers.find(path);
    if (it != impl->buffers.end()) return it->second;

    auto data = Engine::instance().fs().read(path);
    if (data.empty()) return nullptr;

    auto* buf = new SoundBuffer;
    if (!buf->load(data.data(), data.size())) {
        delete buf;
        return nullptr;
    }
    impl->buffers[path] = buf;
    return buf;
}

SoundSource* AudioSystem::createSource(bool persistent, int priority) {
    if (!initialized || cfg.maxSources <= 0)
        return nullptr;
    if (impl->sources.size() >= static_cast<size_t>(cfg.maxSources)) {
        std::vector<AudioSourcePolicy::Candidate> candidates;
        candidates.reserve(impl->sources.size());
        for (const auto* source : impl->sources)
            candidates.push_back({source->priority, source->persistent,
                                  source->paused, source->serial});
        const int victim = AudioSourcePolicy::selectSteal(candidates, priority);
        if (victim < 0) return nullptr;
        releaseSource(impl->sources[static_cast<size_t>(victim)]);
    }
    auto* src = new SoundSource;
    src->persistent = persistent;
    src->priority = priority;
    src->serial = impl->nextSourceSerial++;
    alGenSources(1, &src->source);
    alSourcei(src->source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(src->source, AL_ROLLOFF_FACTOR, 1.0f);
    float effectiveVol = cfg.enabled ? cfg.masterVolume * cfg.sfxVolume : 0.0f;
    src->setVolume(effectiveVol);
    src->setUnderwater(underwater_, impl->hasEfx ? impl->underwaterFilter : 0);
    src->setPitch(playbackRate_);
    impl->sources.push_back(src);
    applyEnvironment();
    return src;
}

bool AudioSystem::isSourceAlive(const SoundSource* source) const {
    return source && std::find(impl->sources.begin(), impl->sources.end(), source) != impl->sources.end();
}

void AudioSystem::releaseSource(SoundSource* source) {
    if (!source) return;
    source->stop();
    source->destroy();
    auto it = std::find(impl->sources.begin(), impl->sources.end(), source);
    if (it != impl->sources.end()) impl->sources.erase(it);
    delete source;
}

void AudioSystem::stopAll() {
    for (auto* src : impl->sources) src->stop();
}

void AudioSystem::pauseAll() {
    for (auto* src : impl->sources)
        if (src && src->isPlaying()) src->pause();
}

void AudioSystem::resumeAll() {
    for (auto* src : impl->sources)
        if (src && src->paused) src->resume();
}

void AudioSystem::setPlaybackRate(float rate) {
    playbackRate_ = std::max(0.01f, rate);
    for (auto* src : impl->sources)
        if (src) src->setPitch(playbackRate_);
}

// SoundBuffer
bool SoundBuffer::load(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    // Check WAV header
    if (memcmp(data, "RIFF", 4) == 0) return loadWav(data, size);
    if (size > 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g') return loadOgg(data, size);
    return loadWav(data, size);
}

bool SoundBuffer::loadWav(const uint8_t* data, size_t size) {
    // Basic WAV loader
    if (size < 44) return false;
    const uint8_t* ptr = data;

    // Skip RIFF header
    uint32_t riffSize = *(uint32_t*)(ptr + 4);
    (void)riffSize;
    ptr += 12;

    // Find fmt chunk
    while (ptr <= data + size - 8) {
        if (memcmp(ptr, "fmt ", 4) == 0) break;
        uint32_t chunkSize = *(uint32_t*)(ptr + 4);
        if (chunkSize > (size_t)(data + size - ptr - 8)) return false;
        ptr += 8 + chunkSize;
    }
    if (ptr >= data + size - 8) return false;

    uint32_t fmtSize = *(uint32_t*)(ptr + 4);
    if (fmtSize < 16 || fmtSize > (size_t)(data + size - ptr - 8)) return false;
    uint16_t format = *(uint16_t*)(ptr + 8);
    uint16_t channels = *(uint16_t*)(ptr + 10);
    uint32_t sampleRate = *(uint32_t*)(ptr + 12);
    uint16_t bitsPerSample = *(uint16_t*)(ptr + 22);
    ptr += 8 + fmtSize;

    // Find data chunk
    while (ptr <= data + size - 8) {
        if (memcmp(ptr, "data", 4) == 0) break;
        uint32_t chunkSize = *(uint32_t*)(ptr + 4);
        if (chunkSize > (size_t)(data + size - ptr - 8)) return false;
        ptr += 8 + chunkSize;
    }
    if (ptr >= data + size - 8) return false;

    uint32_t dataSize = *(uint32_t*)(ptr + 4);
    ptr += 8;

    // Clamp to the bytes actually present in the buffer (attacker-controlled dataSize).
    size_t avail = (data + size > ptr) ? (size_t)(data + size - ptr) : 0;
    if (dataSize > avail) dataSize = (uint32_t)avail;
    if (dataSize == 0) return false;

    ALenum alFormat;
    if (channels == 1 && bitsPerSample == 8) alFormat = AL_FORMAT_MONO8;
    else if (channels == 1 && bitsPerSample == 16) alFormat = AL_FORMAT_MONO16;
    else if (channels == 2 && bitsPerSample == 8) alFormat = AL_FORMAT_STEREO8;
    else if (channels == 2 && bitsPerSample == 16) alFormat = AL_FORMAT_STEREO16;
    else return false;

    if (format != 1) return false; // PCM only

    alGenBuffers(1, &buffer);
    alBufferData(buffer, alFormat, ptr, dataSize, sampleRate);
    const uint32_t bytesPerSecond = sampleRate * channels * (bitsPerSample / 8);
    durationMs = bytesPerSecond ? (uint32_t)((uint64_t)dataSize * 1000 / bytesPerSecond) : 0;
    loaded = true;
    return true;
}

bool SoundBuffer::loadOgg(const uint8_t* data, size_t size) {
    // OGG Vorbis loader using libvorbisfile
    struct MemFile {
        const uint8_t* start;
        const uint8_t* ptr;
        size_t size;
        size_t left;
    };
    MemFile mf = {data, data, size, size};

    ov_callbacks cb;
    cb.read_func = [](void* ptr, size_t sz, size_t nmemb, void* datasource) -> size_t {
        auto* m = (MemFile*)datasource;
        size_t want = sz * nmemb;
        if (want > m->left) want = m->left;
        memcpy(ptr, m->ptr, want);
        m->ptr += want;
        m->left -= want;
        return want;
    };
    cb.seek_func = [](void* datasource, ogg_int64_t offset, int whence) -> int {
        auto* m = (MemFile*)datasource;
        ogg_int64_t base = 0;
        if (whence == SEEK_CUR) base = (ogg_int64_t)(m->ptr - m->start);
        else if (whence == SEEK_END) base = (ogg_int64_t)m->size;
        else if (whence != SEEK_SET) return -1;
        const ogg_int64_t target = base + offset;
        if (target < 0 || (uint64_t)target > m->size) return -1;
        m->ptr = m->start + target;
        m->left = m->size - (size_t)target;
        return 0;
    };
    cb.close_func = [](void*) -> int { return 0; };
    cb.tell_func = [](void* datasource) -> long {
        auto* m = (MemFile*)datasource;
        return (long)(m->ptr - m->start);
    };

    OggVorbis_File vf;
    if (ov_open_callbacks(&mf, &vf, nullptr, 0, cb) < 0) {
        Console::instance().printf(LogLevel::Warn, "Audio: OGG parse failed");
        return false;
    }

    vorbis_info* vi = ov_info(&vf, -1);
    if (!vi) { ov_clear(&vf); return false; }

    const int channels = vi->channels;
    ALenum format = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    int freq = vi->rate;

    // Read all PCM data
    std::vector<int16_t> pcm;
    char readBuf[4096];
    int bitStream = 0;
    long bytesRead;
    while ((bytesRead = ov_read(&vf, readBuf, sizeof(readBuf), 0, 2, 1, &bitStream)) > 0)
        pcm.insert(pcm.end(), (int16_t*)readBuf, (int16_t*)(readBuf + bytesRead));

    ov_clear(&vf);

    if (pcm.empty()) return false;

    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, pcm.data(), (ALsizei)(pcm.size() * sizeof(int16_t)), freq);
    const size_t sampleCount = pcm.size() / (size_t)std::max(1, channels);
    durationMs = freq > 0 ? (uint32_t)(sampleCount * 1000 / (size_t)freq) : 0;
    loaded = (alGetError() == AL_NO_ERROR);
    return loaded;
}

void SoundBuffer::destroy() {
    if (buffer) {
        alDeleteBuffers(1, &buffer);
        buffer = 0;
    }
    loaded = false;
    durationMs = 0;
}

// SoundSource
void SoundSource::play(SoundBuffer* buffer) {
    if (!buffer || !buffer->loaded) return;
    // Replaying a reusable source must first leave AL_INITIAL/AL_STOPPED with
    // its old buffer detached; assigning AL_BUFFER while playing is invalid.
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, buffer->buffer);
    alSourcePlay(source);
    playing = true;
    paused = false;
    scheduledBuffer = buffer;
    scheduledElapsed = 0.0;
    scheduledGap = 0.0;
}

void SoundSource::stop() {
    alSourceStop(source);
    playing = false;
    paused = false;
    scheduledElapsed = 0.0;
    scheduledGap = 0.0;
}

void SoundSource::pause() {
    if (!isPlaying() && (!loopScheduleActive || scheduledGap <= 0.0)) return;
    if (playing) alSourcePause(source);
    paused = true;
    playing = false;
}

void SoundSource::resume() {
    if (!paused) return;
    if (scheduledGap <= 0.0) alSourcePlay(source);
    paused = false;
    playing = scheduledGap <= 0.0;
}

void SoundSource::setVolume(float vol) {
    volume = std::isfinite(vol) ? std::max(0.0f, vol) : 0.0f;
    alSourcef(source, AL_GAIN, volume * (underwater ? AudioSystem::underwaterSourceGain() : 1.0f) *
                                AudioSystem::occlusionGain(occlusion));
}

void SoundSource::setUnderwater(bool value, uint32_t filter) {
    underwater = value;
    alSourcef(source, AL_GAIN, volume * (underwater ? AudioSystem::underwaterSourceGain() : 1.0f) *
                                AudioSystem::occlusionGain(occlusion));
    if (filter) alSourcei(source, AL_DIRECT_FILTER, underwater ? (ALint)filter : AL_FILTER_NULL);
}

void SoundSource::setOcclusion(float amount) {
    occlusion = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 1.0f;
    alSourcef(source, AL_GAIN, volume * (underwater ? AudioSystem::underwaterSourceGain() : 1.0f) *
                                AudioSystem::occlusionGain(occlusion));
}

void SoundSource::setPitch(float p) {
    pitch = std::isfinite(p) ? clampPitch(p) : 1.0f;
    alSourcef(source, AL_PITCH, pitch);
}

void SoundSource::setPosition(const Point3F& pos) {
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
    position = pos;
    alSource3f(source, AL_POSITION, pos.x, pos.y, pos.z);
    setRelative(false);
}

void SoundSource::setVelocity(const Point3F& velocity) {
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
        !std::isfinite(velocity.z)) return;
    this->velocity = velocity;
    alSource3f(source, AL_VELOCITY, velocity.x, velocity.y, velocity.z);
}

void SoundSource::setRelative(bool value) {
    relative = value;
    positional = !value;
    alSourcei(source, AL_SOURCE_RELATIVE, value ? AL_TRUE : AL_FALSE);
}

void SoundSource::setAuxiliarySend(uint32_t slot, int sendIndex, uint32_t filter) {
    if (sendIndex < 0) return;
    auxiliarySend = slot;
    auxiliarySendIndex = sendIndex;
    auxiliaryFilter = filter;
    if (!environmentSend)
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot ? (ALint)slot : AL_EFFECTSLOT_NULL,
                   sendIndex, filter ? (ALint)filter : AL_FILTER_NULL);
}

void SoundSource::setEnvironmentSend(uint32_t slot, bool enabled) {
    environmentSend = enabled ? slot : 0;
    if (environmentSend) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, (ALint)environmentSend, 0, AL_FILTER_NULL);
    } else {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER,
                   auxiliarySend ? (ALint)auxiliarySend : AL_EFFECTSLOT_NULL,
                   auxiliarySendIndex,
                   auxiliaryFilter ? (ALint)auxiliaryFilter : AL_FILTER_NULL);
    }
}

void SoundSource::setDistance(float reference, float maxDistance) {
    referenceDistance = std::max(0.001f, std::isfinite(reference) ? reference : 1.0f);
    this->maxDistance = std::max(referenceDistance,
                                 std::isfinite(maxDistance) ? maxDistance : referenceDistance);
    alSourcef(source, AL_REFERENCE_DISTANCE, referenceDistance);
    alSourcef(source, AL_MAX_DISTANCE, this->maxDistance);
}

void SoundSource::setRolloff(float factor) {
    rolloffFactor = std::isfinite(factor) ? clampRolloff(factor) : 1.0f;
    alSourcef(source, AL_ROLLOFF_FACTOR, rolloffFactor);
}

void SoundSource::setOffsetSeconds(float seconds) {
    offsetSeconds = std::isfinite(seconds) && seconds >= 0.0f ? seconds : 0.0f;
    alSourcef(source, AL_SEC_OFFSET, offsetSeconds);
}

void SoundSource::setLooping(bool loop) {
    looping = loop;
    if (!loop) loopScheduleActive = false;
    alSourcei(source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
}

void SoundSource::setLoopSchedule(int32_t loopCount, int32_t minGapMs,
                                  int32_t maxGapMs, uint32_t deterministicSeed) {
    loopScheduleActive = true;
    looping = true;
    loopRepeatsRemaining = loopCount;
    loopGapMinMs = std::max(0, minGapMs);
    loopGapMaxMs = std::max(loopGapMinMs, maxGapMs);
    loopRandomState = deterministicSeed ? deterministicSeed : 1;
    alSourcei(source, AL_LOOPING, AL_FALSE);
}

void SoundSource::advance(float seconds) {
    if (!loopScheduleActive || paused || !scheduledBuffer ||
        scheduledBuffer->durationMs == 0)
        return;
    double remaining = seconds;
    const double duration = static_cast<double>(scheduledBuffer->durationMs) /
                            (1000.0 * std::max(0.01f, pitch));
    while (remaining > 0.0 && !paused) {
        if (scheduledGap > 0.0) {
            const double step = std::min(remaining, scheduledGap);
            scheduledGap -= step;
            remaining -= step;
            if (scheduledGap > 0.0) continue;
            alSourcePlay(source);
            playing = true;
        }
        const double step = std::min(remaining, duration - scheduledElapsed);
        scheduledElapsed += step;
        remaining -= step;
        if (scheduledElapsed + 1e-9 < duration) continue;
        scheduledElapsed = 0.0;
        if (loopRepeatsRemaining == 0) {
            alSourceStop(source);
            playing = false;
            looping = false;
            loopScheduleActive = false;
            break;
        }
        if (loopRepeatsRemaining > 0) --loopRepeatsRemaining;
        scheduledGap = static_cast<double>(AudioSourcePolicy::nextLoopGapMs(
            loopRandomState, loopGapMinMs, loopGapMaxMs)) / 1000.0;
        alSourceStop(source);
        playing = false;
        if (scheduledGap <= 0.0) {
            alSourcei(source, AL_BUFFER, scheduledBuffer->buffer);
            alSourcePlay(source);
            playing = true;
        }
    }
}

bool SoundSource::isPlaying() const {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

void SoundSource::destroy() {
    if (source) {
        alDeleteSources(1, &source);
        source = 0;
    }
}
