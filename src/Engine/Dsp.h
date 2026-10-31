// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The DSP chain outside the Engine (docs/Plan.md §5.9): what the Engine is given, and what a setting does for the
// signal path, the headroom and the EQ's response curve. The Engine does the arithmetic, so the analysis shows what
// plays.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"
#include "Engine/ImpulseResponse.h"

namespace camel::audio {

// The enabled bands of an EQ that is on; none when it is off.
std::vector<CamelEqBandV1> engineBands(const EqSettings& eq);

// The chain in Engine form. `impulse` is the response at the rate the chain runs at, null while convolution is off.
struct DspChain {
    std::array<uint32_t, 3> order{};
    std::vector<CamelEqBandV1> bands;
    std::optional<CamelCrossfeedV1> crossfeed;
    Impulse impulse;
    // Points into this chain, which must outlive its use.
    CamelDspV1 abi() const;
};

// The chain these settings give, with `impulse` for convolution when it is on.
DspChain dspChain(const EqSettings& eq, const CrossfeedSettings& crossfeed, const ConvolutionSettings& convolution,
                  const DspOrder& order, Impulse impulse);
DspChain dspChain(const Settings& settings, Impulse impulse);
// The EQ alone, for its own curve and headroom.
DspChain eqChain(const EqSettings& eq);

struct DspAnalysis {
    bool active = false;                 // the chain changes the signal, so playback is processed
    double preampDb = 0.0;               // the chain's automatic headroom, 0 dB or less
    std::vector<double> leftDb, rightDb; // the EQ's bands at each frequency asked for, before the preamp
};

// `chain` at `rateHz`, which must be its impulse response's rate. Bands at or above Nyquist do nothing.
DspAnalysis analyzeDsp(const DspChain& chain, uint32_t rateHz, std::span<const double> frequencies = {});

// The nodes of `chain` that change the signal at `rateHz` on `channels`, in order.
std::vector<DspNode> actingNodes(const DspChain& chain, uint32_t rateHz, uint32_t channels);

} // namespace camel::audio
