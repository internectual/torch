#include "audio/mp3_decoder.h"
#include <mpg123.h>
#include <mutex>

struct Mp3Decoder::Handle {
    mpg123_handle* mh = nullptr;
};

namespace {
bool initLibrary() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = mpg123_init() == MPG123_OK; });
    return ok;
}
} // namespace

Mp3Decoder::Mp3Decoder() = default;

Mp3Decoder::~Mp3Decoder() {
    if (handle_) {
        if (handle_->mh) {
            mpg123_close(handle_->mh);
            mpg123_delete(handle_->mh);
        }
        delete handle_;
    }
}

bool Mp3Decoder::open(std::vector<uint8_t> data) {
    if (!initLibrary() || data.empty()) return false;
    data_ = std::move(data);
    handle_ = new Handle;
    int err = 0;
    handle_->mh = mpg123_new(nullptr, &err);
    if (!handle_->mh) return false;
    // Signed 16-bit output at the stream's own rate and channel count.
    mpg123_format_none(handle_->mh);
    const long* rates = nullptr;
    size_t count = 0;
    mpg123_rates(&rates, &count);
    for (size_t i = 0; i < count; ++i)
        mpg123_format(handle_->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    if (mpg123_open_feed(handle_->mh) != MPG123_OK) return false;
    if (mpg123_feed(handle_->mh, data_.data(), data_.size()) != MPG123_OK) return false;
    int encoding = 0;
    // The first frame gives the format.
    unsigned char probe[1];
    size_t done = 0;
    const int result = mpg123_read(handle_->mh, probe, 0, &done);
    if (result != MPG123_NEW_FORMAT && mpg123_getformat(handle_->mh, &rate_, &channels_, &encoding) != MPG123_OK)
        return false;
    if (result == MPG123_NEW_FORMAT) mpg123_getformat(handle_->mh, &rate_, &channels_, &encoding);
    return rate_ > 0 && (channels_ == 1 || channels_ == 2);
}

size_t Mp3Decoder::read(int16_t* out, size_t maxSamples) {
    if (!handle_ || !handle_->mh || finished_) return 0;
    size_t total = 0;
    while (total < maxSamples) {
        size_t done = 0;
        const int result = mpg123_read(handle_->mh, reinterpret_cast<unsigned char*>(out + total),
                                       (maxSamples - total) * sizeof(int16_t), &done);
        total += done / sizeof(int16_t);
        if (result == MPG123_NEW_FORMAT) {
            int encoding = 0;
            mpg123_getformat(handle_->mh, &rate_, &channels_, &encoding);
            continue;
        }
        if (result == MPG123_NEED_MORE || result == MPG123_DONE || result == MPG123_ERR) {
            finished_ = true;
            break;
        }
        if (done == 0) break;
    }
    return total;
}
