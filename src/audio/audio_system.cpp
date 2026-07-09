#include "audio/audio_system.h"
#include "core/console.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <vorbis/vorbisfile.h>
#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cstring>

struct AudioSystem::Impl {
    ALCdevice* device{};
    ALCcontext* context{};
    std::unordered_map<std::string, SoundBuffer*> buffers;
    std::vector<SoundSource*> sources;
};

AudioSystem::AudioSystem() : impl(new Impl) {}
AudioSystem::~AudioSystem() { delete impl; }

bool AudioSystem::init() {
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

    initialized = true;
    return true;
}

void AudioSystem::shutdown() {
    stopAll();
    for (auto& [k, v] : impl->buffers) delete v;
    impl->buffers.clear();

    if (impl->context) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(impl->context);
    }
    if (impl->device) alcCloseDevice(impl->device);
    initialized = false;
}

void AudioSystem::update(const Point3F& listenerPos, const Point3F& listenerVel,
                         const Point3F& listenerForward, const Point3F& listenerUp) {
    if (!initialized) return;
    ALfloat listenerOri[6] = {listenerPos.x, listenerPos.y, listenerPos.z,
                              listenerUp.x, listenerUp.y, listenerUp.z};
    alListener3f(AL_POSITION, listenerPos.x, listenerPos.y, listenerPos.z);
    alListener3f(AL_VELOCITY, listenerVel.x, listenerVel.y, listenerVel.z);
    alListenerfv(AL_ORIENTATION, listenerOri);

    // Clean up one-shot sources that have finished playing
    for (auto it = impl->sources.begin(); it != impl->sources.end(); ) {
        SoundSource* src = *it;
        if (src && !src->looping && !src->isPlaying()) {
            src->destroy();
            delete src;
            it = impl->sources.erase(it);
        } else {
            ++it;
        }
    }
}

SoundBuffer* AudioSystem::loadSound(const char* path) {
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

SoundSource* AudioSystem::createSource() {
    auto* src = new SoundSource;
    alGenSources(1, &src->source);
    float effectiveVol = cfg.enabled ? cfg.masterVolume * cfg.sfxVolume : 0.0f;
    src->setVolume(effectiveVol);
    impl->sources.push_back(src);
    return src;
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
    for (auto* src : impl->sources) src->pause();
}

void AudioSystem::resumeAll() {
    for (auto* src : impl->sources) src->resume();
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
    while (ptr < data + size - 8) {
        if (memcmp(ptr, "fmt ", 4) == 0) break;
        uint32_t chunkSize = *(uint32_t*)(ptr + 4);
        ptr += 8 + chunkSize;
    }
    if (ptr >= data + size - 8) return false;

    uint32_t fmtSize = *(uint32_t*)(ptr + 4);
    uint16_t format = *(uint16_t*)(ptr + 8);
    uint16_t channels = *(uint16_t*)(ptr + 10);
    uint32_t sampleRate = *(uint32_t*)(ptr + 12);
    uint16_t bitsPerSample = *(uint16_t*)(ptr + 22);
    ptr += 8 + fmtSize;

    // Find data chunk
    while (ptr < data + size - 8) {
        if (memcmp(ptr, "data", 4) == 0) break;
        uint32_t chunkSize = *(uint32_t*)(ptr + 4);
        ptr += 8 + chunkSize;
    }
    if (ptr >= data + size - 8) return false;

    uint32_t dataSize = *(uint32_t*)(ptr + 4);
    ptr += 8;

    ALenum alFormat;
    if (channels == 1 && bitsPerSample == 8) alFormat = AL_FORMAT_MONO8;
    else if (channels == 1 && bitsPerSample == 16) alFormat = AL_FORMAT_MONO16;
    else if (channels == 2 && bitsPerSample == 8) alFormat = AL_FORMAT_STEREO8;
    else if (channels == 2 && bitsPerSample == 16) alFormat = AL_FORMAT_STEREO16;
    else return false;

    if (format != 1) return false; // PCM only

    alGenBuffers(1, &buffer);
    alBufferData(buffer, alFormat, ptr, dataSize, sampleRate);
    loaded = true;
    return true;
}

bool SoundBuffer::loadOgg(const uint8_t* data, size_t size) {
    // OGG Vorbis loader using libvorbisfile
    struct MemFile {
        const uint8_t* ptr;
        size_t left;
    };
    MemFile mf = {data, size};

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
        auto* m = (MemFile*)datasource; (void)m;
        // Can't seek in memory without original start - return error
        (void)offset; (void)whence;
        return -1;
    };
    cb.close_func = [](void*) -> int { return 0; };
    cb.tell_func = [](void* datasource) -> long {
        auto* m = (MemFile*)datasource;
        return (long)(m->ptr - m->left); // approximate, not used by ov_read
    };

    OggVorbis_File vf;
    if (ov_open_callbacks(&mf, &vf, nullptr, 0, cb) < 0) {
        Console::instance().printf(LogLevel::Warn, "Audio: OGG parse failed");
        return false;
    }

    vorbis_info* vi = ov_info(&vf, -1);
    if (!vi) { ov_clear(&vf); return false; }

    ALenum format = (vi->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
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
    loaded = (alGetError() == AL_NO_ERROR);
    return loaded;
}

void SoundBuffer::destroy() {
    if (buffer) alDeleteBuffers(1, &buffer);
    loaded = false;
}

// SoundSource
void SoundSource::play(SoundBuffer* buffer) {
    if (!buffer || !buffer->loaded) return;
    alSourcei(source, AL_BUFFER, buffer->buffer);
    alSourcePlay(source);
    playing = true;
}

void SoundSource::stop() {
    alSourceStop(source);
    playing = false;
}

void SoundSource::pause() {
    alSourcePause(source);
    paused = true;
}

void SoundSource::resume() {
    alSourcePlay(source);
    paused = false;
}

void SoundSource::setVolume(float vol) {
    volume = vol;
    alSourcef(source, AL_GAIN, vol);
}

void SoundSource::setPitch(float p) {
    pitch = p;
    alSourcef(source, AL_PITCH, p);
}

void SoundSource::setPosition(const Point3F& pos) {
    position = pos;
    alSource3f(source, AL_POSITION, pos.x, pos.y, pos.z);
    positional = true;
}

void SoundSource::setLooping(bool loop) {
    looping = loop;
    alSourcei(source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
}

bool SoundSource::isPlaying() const {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

void SoundSource::destroy() {
    if (source) alDeleteSources(1, &source);
}
