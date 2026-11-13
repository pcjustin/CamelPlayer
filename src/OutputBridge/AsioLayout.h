// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Interleaved device frames to ASIO's per-channel buffers (docs/Plan.md §5.6, §5.8).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Common/DsdBits.h"

namespace camel::audio {

// Splits `frames` stereo frames of `bytesPerSample` bytes per channel into `left` and `right`. Native DSD has one byte
// per channel per frame; `lsbFirst` reverses its bit order.
inline void splitStereo(const uint8_t* interleaved, size_t frames, uint32_t bytesPerSample, bool lsbFirst, uint8_t* left,
                        uint8_t* right) {
    for (size_t i = 0; i < frames; i++) {
        std::memcpy(left + i * bytesPerSample, interleaved, bytesPerSample);
        std::memcpy(right + i * bytesPerSample, interleaved + bytesPerSample, bytesPerSample);
        interleaved += size_t(2) * bytesPerSample;
    }
    if (lsbFirst)
        for (size_t i = 0; i < frames * bytesPerSample; i++) {
            left[i] = reverseDsdBits(left[i]);
            right[i] = reverseDsdBits(right[i]);
        }
}

} // namespace camel::audio
