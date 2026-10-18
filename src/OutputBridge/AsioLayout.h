// Interleaved device frames to ASIO's per-channel buffers (docs/Plan.md §5.6, §5.8).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace camel::audio {

// ASIOSTDSDInt8LSB1 wants the oldest sample in the least significant bit; the Engine delivers it in the MSB.
constexpr uint8_t reverseDsdBits(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

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
