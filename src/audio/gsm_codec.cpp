#include "audio/gsm_codec.h"

#include <gsm/gsm.h>

#include <algorithm>

namespace TorchGsm {

Encoder::Encoder() : state_(gsm_create()) {}

Encoder::~Encoder() {
    if (state_) gsm_destroy(static_cast<gsm>(state_));
}

bool Encoder::reset() {
    if (state_) gsm_destroy(static_cast<gsm>(state_));
    state_ = gsm_create();
    return state_ != nullptr;
}

bool Encoder::encode(const int16_t* pcm, std::array<uint8_t, EncodedBytesPerFrame>& frame) {
    if (!state_ || !pcm) return false;
    gsm_signal samples[SamplesPerFrame];
    for (int i = 0; i < SamplesPerFrame; ++i) samples[i] = pcm[i];
    gsm_encode(static_cast<gsm>(state_), samples, frame.data());
    return true;
}

Decoder::Decoder() : state_(gsm_create()) {}

Decoder::~Decoder() {
    if (state_) gsm_destroy(static_cast<gsm>(state_));
}

bool Decoder::decode(const uint8_t* frame, std::array<int16_t, SamplesPerFrame>& pcm) {
    if (!state_ || !frame) return false;
    gsm_signal samples[SamplesPerFrame];
    gsm_byte encoded[EncodedBytesPerFrame];
    std::copy(frame, frame + EncodedBytesPerFrame, encoded);
    gsm_decode(static_cast<gsm>(state_), encoded, samples);
    for (int i = 0; i < SamplesPerFrame; ++i) pcm[i] = static_cast<int16_t>(samples[i]);
    return true;
}

} // namespace TorchGsm
