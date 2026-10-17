#include "Engine/verify/FlacMd5.h"

#include <cstring>

namespace camel::audio {

void FlacMd5::update(const int32_t* samples, size_t count) {
    buf_.resize(count * bytes_);
    uint8_t* p = buf_.data();
    for (size_t i = 0; i < count; i++) {
        uint32_t v = uint32_t(samples[i]);
        for (uint32_t b = 0; b < bytes_; b++) *p++ = uint8_t(v >> (8 * b));
    }
    md5_.update(buf_.data(), buf_.size());
}

} // namespace camel::audio
