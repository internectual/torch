#pragma once
// MP3 decoding for the music stream (the shipped Linux client streams the
// music tracks, base/music/<track>.mp3, through its OpenAL stream
// extension). libmpg123 decodes the file to signed 16-bit PCM.
#include <cstddef>
#include <cstdint>
#include <vector>

class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    // The whole file; false when it is not MPEG audio.
    bool open(std::vector<uint8_t> data);
    // Up to `maxSamples` interleaved samples; 0 at the end of the stream.
    size_t read(int16_t* out, size_t maxSamples);
    long rate() const { return rate_; }
    int channels() const { return channels_; }
    bool finished() const { return finished_; }

private:
    struct Handle;
    Handle* handle_ = nullptr;
    std::vector<uint8_t> data_;
    long rate_ = 0;
    int channels_ = 0;
    bool finished_ = false;
};
