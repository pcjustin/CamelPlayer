// Computes the FLAC STREAMINFO MD5 of decoded samples: every sample as a little-endian signed integer
// of bits / 8 bytes, interleaved (FLAC format specification). Only 16/24-bit sources are supported.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "util/Md5.h"

namespace camel::audio {

class FlacMd5 {
public:
    explicit FlacMd5(uint32_t bitsPerSample) : bytes_(0) {
        if (bitsPerSample != 16 && bitsPerSample != 24) throw std::invalid_argument("FLAC MD5 supports 16/24-bit PCM only");
        bytes_ = bitsPerSample / 8;
    }

    // `samples` are right-aligned to the stream's bit depth.
    void update(const int32_t* samples, size_t count);
    std::array<uint8_t, 16> finish() { return md5_.finish(); }

private:
    uint32_t bytes_;
    Md5 md5_;
    std::vector<uint8_t> buf_;
};

} // namespace camel::audio
