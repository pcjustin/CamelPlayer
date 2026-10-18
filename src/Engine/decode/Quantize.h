// Lossy decoders produce float samples; the adapter converts them to the configured PCM depth exactly once
// (docs/Plan.md §2). Rounds to nearest and clips to full scale; dither arrives with the shared quantization stage.
#pragma once
#include <cmath>
#include <cstdint>

namespace camel::audio {

inline int32_t quantize(double sample, uint32_t bits) {
    const double full = std::ldexp(1.0, int(bits) - 1);
    const double scaled = std::nearbyint(sample * full);
    if (std::isnan(scaled)) return 0;
    if (scaled < -full) return int32_t(-full);
    if (scaled > full - 1) return int32_t(full - 1);
    return int32_t(scaled);
}

} // namespace camel::audio
