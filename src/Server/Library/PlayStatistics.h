// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

// A library track is its file (one track per file since CUE sheets are not supported).
struct LibraryTrackKey {
    int64_t file = 0;
    bool operator==(const LibraryTrackKey&) const = default;
};

// One play of a library track (docs/Plan.md §7.8). Positions are source frames from the start of the file; only
// frames inside [rangeFirst, rangeEnd) belong to the track.
class PlaySession {
public:
    PlaySession(LibraryTrackKey track, uint32_t sampleRate, uint64_t length, uint64_t rangeFirst = 0,
                std::optional<uint64_t> rangeEnd = std::nullopt);

    // Credits the frames since the previous observation when playback moved forward by at most `maxStep` frames.
    void observe(uint64_t position, uint64_t maxStep);
    // The next observation starts a new span: seeks, pauses and stops credit nothing for the jump.
    void interrupt();
    // The track played to its end: the rest of its range is heard and an unknown length counts.
    void finish();
    // Reset removed this track's data; later persistence starts a new event from zero.
    void discard();
    // Queues writing heard time and the count when they changed since the last call.
    void persist(LibraryStore& store);

    const LibraryTrackKey& track() const { return track_; }
    std::optional<uint64_t> position() const { return last_; } // the last observed position while playing
    uint64_t heardMs() const;
    bool counted() const { return counted_; }

private:
    void credit(uint64_t from, uint64_t to);

    LibraryTrackKey track_;
    uint32_t sampleRate_;
    uint64_t length_, rangeFirst_;
    std::optional<uint64_t> rangeEnd_;
    std::optional<uint64_t> last_;
    uint64_t heard_ = 0;
    bool counted_ = false;
    int64_t startedMs_ = 0;
    int32_t utcOffsetMinutes_ = 0;
    uint64_t persistedMs_ = 0;
    bool persistedCount_ = false;
    std::shared_ptr<int64_t> event_ = std::make_shared<int64_t>(0); // assigned by the writer thread
};

struct PlayPeriod {
    std::optional<int64_t> fromMs, toMs; // UTC milliseconds, [from, to); absent bounds are open
};

enum class PlayMetric : uint8_t { Plays, ListeningTime };
enum class PlayBucketSize : uint8_t { Day, Week, Month };

struct PlayStatisticsQuery {
    PlayPeriod period;
    PlayMetric metric = PlayMetric::Plays;
    PlayBucketSize bucket = PlayBucketSize::Day;
    int64_t topLimit = 100;
    int64_t historyOffset = 0, historyLimit = 100;
};

struct PlayOverview {
    int64_t listenedMs = 0, plays = 0, tracks = 0, albums = 0, artists = 0;
};

struct PlayRankedTrack {
    LibraryTrackKey track;
    std::string title, artist, album;
    int64_t plays = 0, listenedMs = 0;
    bool available = false;
};

struct PlayRankedGroup {
    int64_t album = 0; // album id; 0 for artists
    std::string name, artist;
    int64_t plays = 0, listenedMs = 0;
};

struct PlayBucket {
    std::string start; // local date YYYY-MM-DD of the day, the Monday of the week or the first of the month
    int64_t plays = 0, listenedMs = 0;
};

struct PlayHistoryEntry {
    int64_t event = 0;
    LibraryTrackKey track;
    int64_t startedMs = 0;
    int32_t utcOffsetMinutes = 0;
    int64_t heardMs = 0;
    bool counted = false;
    std::string title, artist, album;
    bool available = false;
};

struct PlayStatistics {
    PlayOverview overview;
    std::vector<PlayRankedTrack> tracks;
    std::vector<PlayRankedGroup> albums, artists;
    std::vector<PlayBucket> buckets;
    int64_t historyTotal = 0;
    std::vector<PlayHistoryEntry> history;
};

// Days are local to the UTC offset recorded with each play; artists with several values are each credited in full.
PlayStatistics playStatistics(const LibraryStore& store, const PlayStatisticsQuery& query, const CancellationToken& token = {});
// Deletes the plays and zeroes the counters of these tracks (all tracks when `tracks` is empty); not undoable.
void resetPlayStatistics(LibraryStore& store, const std::vector<LibraryTrackKey>& tracks);

} // namespace camel::server
