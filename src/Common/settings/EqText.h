// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// EQ presets as Equalizer APO text, the format AutoEq publishes as ParametricEQ.txt (docs/Plan.md §5.9):
//   Preamp: -6.2 dB
//   Channel: L
//   Filter 1: ON PK Fc 105 Hz Gain -2.3 dB Q 0.70
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "Common/settings/Settings.h"

namespace camel {

// The filters of `text`: PK, LS/LSC, HS/HSC, LP/LPQ, HP/HPQ, BP, NO and AP, with "Channel: L", "R" or "ALL" applying
// to the filters that follow. OFF filters are kept disabled; lines it does not know, and the preamp (CamelPlayer sets
// its own headroom), are ignored.
std::vector<EqBand> parseEqualizerApo(std::string_view text);

// `bands` as Equalizer APO text with `preampDb` as its preamp. A steeper low- or high-pass than 12 dB/oct has no APO
// filter and is written as a 12 dB/oct one.
std::string formatEqualizerApo(const std::vector<EqBand>& bands, double preampDb);

} // namespace camel
