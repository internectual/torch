// Mp3Decoder: a one-second 440 Hz tone, encoded by lame when the tool is
// present, decodes to about a second of 16-bit PCM at its rate.
#include "audio/mp3_decoder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    if (std::system("command -v lame > /dev/null 2>&1") != 0) {
        std::printf("mp3_decoder_test: lame not installed, skipped\n");
        return 0;
    }
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = tmp && *tmp ? tmp : "/tmp";
    const std::string wav = dir + "/torch_mp3_test.wav", mp3 = dir + "/torch_mp3_test.mp3";
    {
        const int rate = 44100, samples = rate;
        std::ofstream out(wav, std::ios::binary);
        auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4); u32(36 + samples * 2); out.write("WAVEfmt ", 8);
        u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
        out.write("data", 4); u32(samples * 2);
        for (int i = 0; i < samples; ++i) u16((uint16_t)(int16_t)(12000 * std::sin(2 * M_PI * 440 * i / rate)));
    }
    assert(std::system(("lame --quiet -b 128 '" + wav + "' '" + mp3 + "'").c_str()) == 0);
    std::ifstream in(mp3, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Mp3Decoder decoder;
    assert(decoder.open(data));
    assert(decoder.rate() == 44100 && decoder.channels() == 1);
    std::vector<int16_t> pcm(8192);
    size_t total = 0;
    int peak = 0;
    while (size_t n = decoder.read(pcm.data(), pcm.size())) {
        total += n;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs((int)pcm[i]));
    }
    assert(decoder.finished());
    // lame pads the start and end by a frame or two.
    assert(total >= 44100 && total < 44100 + 4 * 1152);
    assert(peak > 8000 && peak < 16000);
    Mp3Decoder notAudio;
    assert(!notAudio.open(std::vector<uint8_t>(4096, 0x41)));
    std::remove(wav.c_str());
    std::remove(mp3.c_str());
    return 0;
}
