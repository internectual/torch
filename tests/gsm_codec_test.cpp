#include "audio/gsm_codec.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstdint>

int main() {
    std::array<int16_t, TorchGsm::SamplesPerFrame> source{};
    for (int i = 0; i < TorchGsm::SamplesPerFrame; ++i)
        source[i] = static_cast<int16_t>(((i * 97) % 2000) - 1000);

    TorchGsm::Encoder encoder;
    TorchGsm::Decoder decoder;
    std::array<uint8_t, TorchGsm::EncodedBytesPerFrame> encoded{};
    std::array<int16_t, TorchGsm::SamplesPerFrame> decoded{};
    assert(encoder.encode(source.data(), encoded));
    assert(decoder.decode(encoded.data(), decoded));

    bool nonzero = false;
    for (uint8_t byte : encoded) nonzero |= byte != 0;
    assert(nonzero);

    int64_t error = 0;
    for (int i = 0; i < TorchGsm::SamplesPerFrame; ++i) {
        error += std::abs(static_cast<int>(source[i]) - static_cast<int>(decoded[i]));
    }
    assert(error > 0);
    assert(error < 300000);
    return 0;
}
