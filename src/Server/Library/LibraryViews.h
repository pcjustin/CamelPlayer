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
// Fixed smart lists (docs/Plan.md §8.2): added in the last 30 days, the 100 most played, the 100 last played, favorites
// (the latest first) and never counted as played (the latest added first).
enum class SmartList : uint8_t { RecentlyAdded, MostPlayed, RecentlyPlayed, Favorites, NeverPlayed };

struct LibraryGroupFilter {
    LibraryGroupField field = LibraryGroupField::Artist;
    std::string value; // the year as decimal text for Year
    bool operator==(const LibraryGroupFilter&) const = default;
};

struct LibraryAlbum {
    int64_t id = 0, folder = 0;
    std::string title, artist;
    int64_t year = 0, tracks = 0, discs = 0, added = 0, durationMs = 0;
    bool favorite = false;
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
    std::string name; // the file's on-disk name, empty until a scan records it
    bool favorite = false;
};

struct LibraryTrackRowPage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<LibraryTrackRow> tracks;
};

// One combination of format, bit depth and sample rate among an album's tracks.
struct LibraryAlbumFormat {
    std::string format; // as tracks.format: flac, mp3, dsd and so on
    int64_t bitsPerSample = 0, sampleRate = 0, tracks = 0;
    bool operator==(const LibraryAlbumFormat&) const = default;
};

struct LibraryAlbumDetail {
    uint64_t revision = 0;
    LibraryAlbum album;
    std::vector<LibraryTrackRow> tracks;     // disc, then track order
    std::vector<LibraryAlbumFormat> formats; // the most common first
};

struct LibraryFolderEntry {
    int64_t id = 0;
    std::string name, path;
    std::string location; // roots only: libraryLocation of the folder
};

struct LibraryFolderListing {
    uint64_t revision = 0;
    std::optional<int64_t> folder;           // absent: the online roots
    std::vector<LibraryFolderEntry> trail;   // the root down to `folder`
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
// Visible albums by their latest play, newest first (docs/Plan.md §8.2 Home). Plays do not change the library
// revision, so these pages are read fresh rather than from a cached order.
LibraryAlbumPage recentlyPlayedAlbums(const LibraryStore& store, int64_t offset, int64_t limit,
                                      const CancellationToken& token = {});

LibraryGroupPage browseGroups(const LibraryStore& store, LibraryGroupField field, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
LibraryTrackRowPage browseTrackRows(const LibraryStore& store, TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                    std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
LibraryTrackRowPage smartTracks(const LibraryStore& store, SmartList list, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
// Favorite albums, the latest marked first.
LibraryAlbumPage favoriteAlbums(const LibraryStore& store, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
LibraryAlbumDetail albumDetail(const LibraryStore& store, int64_t album, const CancellationToken& token = {});
// Shows the names and root locations scans recorded, without reading the music storage.
LibraryFolderListing browseFolder(const LibraryStore& store, std::optional<int64_t> folder, const CancellationToken& token = {});
std::vector<LibraryRootInfo> libraryRoots(const LibraryStore& store);

// A stored root key as people write it: the volume or share's own spelling without the extended-length prefix.
std::string libraryDisplayPath(const std::string& key);
// Where a library folder is, as Roon shows it: a share as \\server\share\folder, a local folder as
// "COMPUTER › [D:] volume label › folder".
std::string libraryLocation(const std::string& key);

} // namespace camel::server
