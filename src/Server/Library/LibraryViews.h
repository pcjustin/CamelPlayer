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
#include "Server/Library/PlayStatistics.h"

// Paged Server queries behind the Control point browse views (docs/Plan.md §8.2). Only available tracks under online,
// registered roots are listed; a nonzero expected revision makes paging fail once a scan or edit changes the library.
namespace camel::server {

enum class LibraryGroupField : uint8_t { Artist, Composer, Genre, Year };
enum class AlbumSort : uint8_t { Title, Artist, Year, Added };
enum class TrackSort : uint8_t { Title, Artist, Album, Year, Duration, Added };

struct LibraryGroupFilter {
    LibraryGroupField field = LibraryGroupField::Artist;
    std::string value; // the year as decimal text for Year
    bool operator==(const LibraryGroupFilter&) const = default;
};

struct LibraryAlbum {
    int64_t id = 0, folder = 0;
    std::string title, artist;
    int64_t year = 0, tracks = 0, discs = 0, added = 0, durationMs = 0;
};

struct LibraryAlbumPage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<LibraryAlbum> albums;
};

struct LibraryGroup {
    std::string name;
    int64_t albums = 0, tracks = 0;
};

struct LibraryGroupPage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<LibraryGroup> groups;
};

struct LibraryTrackRow {
    int64_t id = 0;
    LibraryTrackKey track;
    std::string title, artist, album, path, format;
    int64_t year = 0, disc = 0, number = 0, durationMs = 0;
};

struct LibraryTrackRowPage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<LibraryTrackRow> tracks;
};

struct LibraryAlbumDetail {
    uint64_t revision = 0;
    LibraryAlbum album;
    std::vector<LibraryTrackRow> tracks; // disc, then track order
};

struct LibraryFolderEntry {
    int64_t id = 0;
    std::string name, path;
    std::string location; // roots only: libraryLocation of the folder
};

struct LibraryFolderListing {
    uint64_t revision = 0;
    std::optional<int64_t> folder; // absent: the online roots
    std::vector<LibraryFolderEntry> trail; // the root down to `folder`
    std::vector<LibraryFolderEntry> folders; // only folders with tracks somewhere below them

    std::vector<LibraryTrackRow> tracks;
};

struct LibraryRootInfo {
    int64_t id = 0;
    std::string path, name, location; // name and location as in LibraryFolderEntry
    bool online = false;
    int64_t lastScan = 0;
};

LibraryAlbumPage browseAlbums(const LibraryStore& store, AlbumSort sort, const std::optional<LibraryGroupFilter>& filter,
                              int64_t offset, int64_t limit, std::optional<uint64_t> expectedRevision = std::nullopt,
                              const CancellationToken& token = {});
LibraryGroupPage browseGroups(const LibraryStore& store, LibraryGroupField field, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
LibraryTrackRowPage browseTrackRows(const LibraryStore& store, TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                    std::optional<uint64_t> expectedRevision = std::nullopt,
                                    const CancellationToken& token = {});
LibraryAlbumDetail albumDetail(const LibraryStore& store, int64_t album, const CancellationToken& token = {});
// Reads each folder's on-disk spelling, since stored paths are lowercase keys; call it off the UI thread.
LibraryFolderListing browseFolder(const LibraryStore& store, std::optional<int64_t> folder, const CancellationToken& token = {});
std::vector<LibraryRootInfo> libraryRoots(const LibraryStore& store);

// The on-disk spelling of a stored lowercase path's last component, or the stored component when unreadable.
std::string libraryDisplayName(const std::string& key);
// A stored root key as people write it: the volume or share's own spelling without the extended-length prefix.
std::string libraryDisplayPath(const std::string& key);
// Where a library folder is, as Roon shows it: a share as \\server\share\folder, a local folder as
// "COMPUTER › [D:] volume label › folder".
std::string libraryLocation(const std::string& key);

} // namespace camel::server
