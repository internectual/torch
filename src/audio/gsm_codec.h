#pragma once

#include <array>
#include <cstdint>

namespace TorchGsm {

constexpr int SamplesPerFrame = 160;
constexpr int BytesPerFrame = 320;
constexpr int EncodedBytesPerFrame = 33;

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool encode(const int16_t* pcm, std::array<uint8_t, EncodedBytesPerFrame>& frame);

private:
    void* state_ = nullptr;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool decode(const uint8_t* frame, std::array<int16_t, SamplesPerFrame>& pcm);

private:
    void* state_ = nullptr;
};

} // namespace TorchGsm
