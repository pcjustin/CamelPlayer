// MVP output-format rule (docs/計劃書.md §16.2): integer zero padding only, never conversion.
// The full FormatNegotiator (M1-07) replaces this later.
#pragma once
#include <functional>
#include <optional>
#include <vector>

#include "audio/AudioTypes.h"

namespace camel::audio {

// Containers tried for a source of `bits` bits, smallest first: 16 -> 24 -> 24-in-32 -> 32.
std::vector<SampleType> bitPerfectCandidates(uint32_t bits);

// Picks the first candidate the device accepts. `fixedType` (from settings) restricts the choice to that one layout.
// Returns nullopt when nothing fits; the caller then skips the track and tells the user.
std::optional<DeviceFormat> chooseOutputFormat(const SourceInfo& src, std::optional<SampleType> fixedType,
                                               const std::function<bool(const DeviceFormat&)>& deviceSupports);

}  // namespace camel::audio
