// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Server/Api/ServerApi.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {

// A zone's queue as it is saved between runs in the `queue` table under its zone's ID (docs/Plan.md §7, §5.12, M5-R01);
// positions index `entries`.
struct SavedQueueEntry {
    TrackSource source;
    int64_t file = 0;        // the library file, followed through identity moves; 0 outside the library
    std::string description; // title and artist, shown when the library no longer describes the file
    bool automatic = false;
    bool operator==(const SavedQueueEntry&) const = default;
};

// The current entry as the playback bar showed it; restored paused, and its file opens only when it plays.
struct SavedTrack {
    uint64_t position = 0, total = 0;
    std::optional<audio::SourceInfo> source;
    std::optional<audio::AudioFormat> format;
    bool operator==(const SavedTrack&) const = default;
};

struct SavedSession {
    std::vector<SavedQueueEntry> entries;
    std::vector<size_t> order, history;
    std::optional<size_t> current;
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    SavedTrack track;
};

// Each part is written only when asked, since a large queue makes its entries and play order costly to rewrite. Entries
// before `keptEntries` are left as they are stored, unless a row of them has gone from the table.
struct SessionParts {
    bool entries = false, order = false;
    size_t keptEntries = 0;
};

// Alike for two entries whose rows are stored alike.
size_t rowHash(const SavedQueueEntry& entry);

// The zone ID that holds the one queue saved before zones, until the Server gives it to a zone.
inline constexpr const char* kQueueBeforeZones = "";

void saveSession(sqlite::Database& db, const std::string& zone, const SavedSession& session, SessionParts parts);
// Whether any zone, or the queue saved before zones, has one.
bool hasSavedSession(sqlite::Database& db);
bool hasSavedSession(sqlite::Database& db, const std::string& zone);
// Nothing when the zone has no saved session or it cannot be read.
std::optional<SavedSession> loadSession(sqlite::Database& db, const std::string& zone);
void deleteSession(sqlite::Database& db, const std::string& zone);
// The session saved under `from` becomes `to`'s, whose own goes.
void moveSession(sqlite::Database& db, const std::string& from, const std::string& to);

} // namespace camel::server
