// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/verify/FlacMd5.h"

#include <cstring>

namespace camel::audio {

void FlacMd5::update(const int32_t* samples, size_t count) {
    buf_.resize(count * bytes_);
    uint8_t* p = buf_.data();
    for (size_t i = 0; i < count; i++) {
        auto v = uint32_t(samples[i]);
        for (uint32_t b = 0; b < bytes_; b++) *p++ = uint8_t(v >> (8 * b));
    }
    md5_.update(buf_.data(), buf_.size());
}

} // namespace camel::audio
