// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Impulse responses for convolution (docs/Plan.md §5.9): read from a .wav, or from a .zip holding one .wav per sample
// rate, and prepared for the Engine at the rate that plays. A missing rate is converted from the nearest one with
// soxr and cached under the data folder, keyed by the file's content and the rate.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Engine/EngineAbi.h"

namespace camel::audio {

// A response prepared for one rate, shared by engines and analyses.
using Impulse = std::shared_ptr<CamelImpulse>;

// The longest response the Engine is given, per path at the playing rate: about 23.8 s at 44.1 kHz.
inline constexpr uint64_t kMaxImpulseTaps = uint64_t(1) << 20;

struct ImpulseInfo {
    uint32_t channels = 0;       // 1 for both outputs, 2 for left and right, 4 for true stereo
    uint64_t frames = 0;         // at the playing rate
    std::vector<uint32_t> rates; // the rates the file holds
    uint32_t sourceRate = 0;     // the rate the response was taken from
    bool converted = false;      // resampled to the playing rate
};

struct LoadedImpulse {
    Impulse impulse;
    ImpulseInfo info;
};

enum class ImpulseProblem { Unreadable, Layout, TooLong };

class ImpulseError : public std::runtime_error {
public:
    ImpulseError(ImpulseProblem problem, const std::string& detail) : std::runtime_error(detail), problem_(problem) {}
    ImpulseProblem problem() const { return problem_; }

private:
    ImpulseProblem problem_;
};

// `file` prepared for `rateHz`. Repeated calls for an unchanged file reuse what earlier ones prepared. Throws
// ImpulseError when the file cannot be used.
LoadedImpulse loadImpulse(const std::filesystem::path& file, uint32_t rateHz);

} // namespace camel::audio
