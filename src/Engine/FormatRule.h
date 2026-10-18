// Output format negotiation (docs/Plan.md §5.3) and what the chosen path means for the indicator (§5.2).
#pragma once
#include <functional>
#include <optional>
#include <vector>

#include "Common/AudioTypes.h"

namespace camel::audio {

// Bit-perfect: every source bit reaches the device (zero padding, mono duplication). Lossless conversion: integers
// widened to float32 for a float-only device. Processed: resampled, downmixed or reduced in depth. Lossy source: an
// AAC/MP3 decode passed on unchanged otherwise.
enum class PipelineState : uint8_t { BitPerfect, LosslessConversion, Processed, LossySource };

struct OutputPlan {
    DeviceFormat format;
    bool duplicate = false; // mono to both channels
    bool downmix = false;   // ITU-R BS.775 to stereo
    bool resample = false;
    bool widen = false;  // integers to float32
    bool reduce = false; // fewer bits than the source, with dither
    PipelineState state = PipelineState::BitPerfect;
    bool operator==(const OutputPlan&) const = default;
};

// Containers tried for a source of `bits` bits, smallest first: 16 -> 24 -> 24-in-32 -> 32.
std::vector<SampleType> bitPerfectCandidates(uint32_t bits);

// Rates are tried in order: the source rate, then with `convert` the lowest same-family rate at or above it, the
// lowest other-family rate at or above it, and the highest rate. Each rate tries containers that carry the source
// depth, then float32, then with `convert` a smaller depth. Output is always stereo: mono is duplicated, more channels
// need `convert`. `fixedType` (from settings) restricts the container. `lossy` marks a float decode (AAC/MP3).
// Returns nullopt when nothing fits; the caller then skips the track and tells the user.
std::optional<OutputPlan> negotiateOutput(const SourceInfo& src, bool lossy, std::optional<SampleType> fixedType, bool convert,
                                          const std::function<bool(const DeviceFormat&)>& deviceSupports);

} // namespace camel::audio
