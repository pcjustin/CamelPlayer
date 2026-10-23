// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

struct LibraryTrack {
    int64_t id = 0;
    std::string title, artist, album, path;
};

struct LibraryTrackPage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<LibraryTrack> tracks;
};

// A nonzero expected revision makes paging fail once a scan or edit changes the result set.
LibraryTrackPage browseTracks(const LibraryStore& store, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision = std::nullopt,
                              const CancellationToken& token = {});
LibraryTrackPage searchTracks(const LibraryStore& store, std::string query, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision = std::nullopt,
                              const CancellationToken& token = {});

} // namespace camel::server
