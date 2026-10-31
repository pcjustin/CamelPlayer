// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ReplayGain for a track (docs/Plan.md §5.9): its tags first, then the library's analysis, by mode, preamp and peak.
#pragma once

#include <cstdint>
#include <optional>

#include "Common/settings/Settings.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {

// A track's loudness: gains in dB to the -18 LUFS reference, peaks as a fraction of full scale.
struct TrackLoudness {
    std::optional<double> trackGain, trackPeak, albumGain, albumPeak;
};

// The gain in dB for a track. `albumContext` tells Auto that the queue plays the track's album in order.
double replayGainDb(const ReplayGainSettings& settings, const TrackLoudness& loudness, bool albumContext);

// What the library holds for the track of `fileId`: each value from the tags, else from the analysis.
TrackLoudness trackLoudness(sqlite::Database& db, int64_t fileId);

} // namespace camel::server
