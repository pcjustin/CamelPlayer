// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/ReplayGain.h"

#include <algorithm>
#include <cmath>

namespace camel::server {

double replayGainDb(const ReplayGainSettings& settings, const TrackLoudness& loudness, bool albumContext) {
    if (settings.mode == ReplayGainMode::Off) return 0.0;
    const bool album = settings.mode == ReplayGainMode::Album || (settings.mode == ReplayGainMode::Auto && albumContext);
    // An album without a value of its own falls back to the track's.
    const bool useAlbum = album && loudness.albumGain;
    const auto gain = useAlbum ? loudness.albumGain : loudness.trackGain;
    const auto peak = useAlbum ? loudness.albumPeak : loudness.trackPeak;
    if (!gain) return settings.noValuePreampDb;
    double db = *gain + settings.preampDb;
    if (settings.preventClipping && peak && *peak > 0.0) db = std::min(db, -20.0 * std::log10(*peak));
    return db;
}

TrackLoudness trackLoudness(sqlite::Database& db, int64_t fileId) {
    TrackLoudness loudness;
    auto row = db.prepare("SELECT coalesce(rg_track_gain,rg_analysis_gain),coalesce(rg_track_peak,rg_analysis_peak),"
                          "coalesce(rg_album_gain,rg_analysis_album_gain),coalesce(rg_album_peak,rg_analysis_album_peak) "
                          "FROM tracks WHERE file_id=?");
    if (!row.bind(1, fileId).step()) return loudness;
    const auto value = [&](int column) { return row.isNull(column) ? std::nullopt : std::optional(row.real(column)); };
    loudness.trackGain = value(0);
    loudness.trackPeak = value(1);
    loudness.albumGain = value(2);
    loudness.albumPeak = value(3);
    return loudness;
}

} // namespace camel::server
