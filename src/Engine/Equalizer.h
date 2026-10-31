// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The parametric EQ outside the Engine (docs/Plan.md §5.9): the bands the Engine is given, and what a setting does
// for the signal path and the response curve. The Engine does the arithmetic, so the curve shows what plays.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

// The enabled bands of an EQ that is on; none when it is off.
std::vector<CamelEqBandV1> engineBands(const EqSettings& eq);

struct EqAnalysis {
    bool active = false;                 // the EQ changes the signal, so playback is processed
    double preampDb = 0.0;               // the automatic headroom, 0 dB or less
    std::vector<double> leftDb, rightDb; // the level at each frequency asked for, preamp included
};

// `eq` at `rateHz`. Bands at or above the Nyquist frequency of that rate do nothing.
EqAnalysis analyzeEq(const EqSettings& eq, uint32_t rateHz, std::span<const double> frequencies = {});

} // namespace camel::audio
