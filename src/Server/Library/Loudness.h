// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Loudness analysis for ReplayGain (docs/Plan.md §5.9): EBU R128 integrated loudness and true peak with libebur128,
// for tracks whose tags hold no value, an album at a time. It runs in the background only while ReplayGain is on,
// queued tracks first, and keeps its results in library.db, never in the files.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "Common/settings/Settings.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

// ReplayGain 2.0's reference loudness.
inline constexpr double kReferenceLufs = -18.0;

struct Loudness {
    double gainDb = 0.0; // to the reference
    double peak = 0.0;   // true peak, a fraction of full scale
};

struct AlbumLoudness {
    std::vector<std::optional<Loudness>> tracks; // per file; none for a file that could not be read
    std::optional<Loudness> album;               // over every file read
};

// An album's files measured one at a time. A run cut short keeps the files it finished and the next run goes on from
// there. The album's value comes from a histogram of every file's 400 ms blocks, so no file's measurement is kept
// open, however long the album. A mono file counts as dual mono, as it plays on both speakers.
class AlbumMeasure {
public:
    explicit AlbumMeasure(std::vector<std::filesystem::path> files);

    // Decodes the files not yet done with the decoders `decoders` choose, on up to `threads` threads; `cancelled` stops
    // the reading between chunks. `measured(i, loudness)` is called for each file as it finishes, one at a time, with no
    // value when it could not be read. Returns whether every file is done.
    bool run(const DecoderByFormat& decoders, unsigned threads, const std::function<bool()>& cancelled = {},
             const std::function<void(size_t, const std::optional<Loudness>&)>& measured = {});

    AlbumLoudness result() const; // the files done so far and the album over them

private:
    std::vector<std::filesystem::path> files_;
    std::vector<bool> done_;
    std::vector<std::optional<Loudness>> tracks_;
    std::array<uint64_t, 1000> blocks_{}; // libebur128's histogram: -70 to +30 LUFS in 0.1 LU steps
    double peak_ = 0.0;
    mutable std::mutex mutex_;
};

// Analyzes the library's tracks that lack a track or album value, while the settings ask for ReplayGain.
class LoudnessAnalyzer {
public:
    struct Options {
        std::function<ReplayGainSettings()> settings;
        std::function<DecoderByFormat()> decoders;
        std::function<std::vector<int64_t>()> queued;         // file ids of the current and upcoming queue entries
        std::function<std::filesystem::path()> playingSource; // the file playing now; empty when none
    };

    LoudnessAnalyzer(LibraryStore& store, Options options);
    ~LoudnessAnalyzer();
    LoudnessAnalyzer(const LoudnessAnalyzer&) = delete;
    LoudnessAnalyzer& operator=(const LoudnessAnalyzer&) = delete;

    LoudnessProgress progress() const;

private:
    struct Track {
        int64_t file = 0;
        std::filesystem::path path;
    };
    void run(const std::stop_token& stop);
    std::vector<Track> nextAlbum();
    void count();

    LibraryStore& store_;
    Options options_;
    std::set<int64_t> failed_;                // files that could not be read this session
    std::unique_ptr<AlbumMeasure> measuring_; // the album cut short last, with its files' ids
    std::vector<int64_t> measuringFiles_;
    std::atomic<int64_t> analyzed_ = 0, total_ = 0;
    std::atomic<bool> running_ = false;
    std::jthread thread_;
};

} // namespace camel::server
