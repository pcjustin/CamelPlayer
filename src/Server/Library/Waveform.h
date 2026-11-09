// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A track's waveform for the playback bar (docs/Task-breakdown.md M6.6-R07): the loudest sample of each of
// kWaveformPeaks equal stretches of the track, scaled to the track's own loudest, 0 to 255.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "Common/settings/Settings.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {

inline constexpr size_t kWaveformPeaks = 800;

// Takes decoded frames as they come and keeps the loudest sample of every 10 ms, which `peaks` spreads over the
// track's length once it is known.
class PeakCollector {
public:
    PeakCollector(uint32_t sampleRate, unsigned channels);
    void add(const float* interleaved, size_t frames); // full scale 1.0
    // kWaveformPeaks values; all zero for silence, none when nothing was added.
    std::vector<uint8_t> peaks() const;

private:
    size_t blockFrames_;
    unsigned channels_;
    size_t inBlock_ = 0;
    float current_ = 0.0f;
    std::vector<float> blocks_;
};

// Decodes `file` with the decoders `decoders` choose, DSD through playback's DSD-to-PCM decimator, and returns its
// peaks; none when it cannot be read or `cancelled` stopped the reading between chunks.
std::optional<std::vector<uint8_t>> readWaveform(const std::filesystem::path& file, const DecoderByFormat& decoders,
                                                 const std::function<bool()>& cancelled = {});

void storeWaveform(sqlite::Database& db, int64_t file, const std::vector<uint8_t>& peaks);
std::optional<std::vector<uint8_t>> storedWaveform(sqlite::Database& db, int64_t file);

} // namespace camel::server
