// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryNames.h"
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

// Which albums of an artist or composer an album listing holds: every album with one of their tracks, the albums whose
// album artist credits them, or the other albums with one of their tracks.
enum class LibraryAlbumRole : uint8_t { Any, Own, Guest };

struct LibraryGroupFilter {
    LibraryGroupField field = LibraryGroupField::Artist;
    std::string value;                             // the year as decimal text for Year
    LibraryAlbumRole role = LibraryAlbumRole::Any; // album listings of artists and composers only
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
    bool favorite = false;                               // artists and composers only
    LibraryGroupField field = LibraryGroupField::Artist; // favoriteArtists lists both fields
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

// An artist's or composer's page (docs/Plan.md §8.2): the tracks credited to them, the most played first, then their
// own albums and the other albums with some of their tracks, the newest first.
struct LibraryArtistPage {
    uint64_t revision = 0;
    int64_t tracks = 0;                 // every visible track credited to them
    std::vector<LibraryTrackRow> songs; // the most played, then by year and album order
    LibraryAlbumPage albums, appearances;
    bool favorite = false;
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

// `favoritesOnly` keeps the favorite artists or composers.
LibraryGroupPage browseGroups(const LibraryStore& store, LibraryGroupField field, bool favoritesOnly, int64_t offset,
                              int64_t limit, std::optional<uint64_t> expectedRevision = std::nullopt,
                              const CancellationToken& token = {});

// Where an initial starts in a whole list, for the jump bar beside it: "#" (digits and symbols), "A"–"Z", "あ" (kana)
// and "漢" (Han, Hangul and the scripts sorted after them). Initials without names are left out; a script sorted between
// two initials joins the earlier one.
struct LibraryInitial {
    std::string label;
    int64_t offset = 0;
    bool operator==(const LibraryInitial&) const = default;
};
// Every initial in jump bar order.
const std::vector<std::string>& libraryInitialLabels();
// What showing artist credits needs, read once per library revision: the names the library knows, and the artist names
// the Artists list holds.
struct LibraryCreditNames {
    uint64_t revision = 0;
    LibraryNameSplitter splitter;
    std::unordered_set<std::string> artists;
};
// Reads them again unless `cached` belongs to the current library revision.
std::shared_ptr<const LibraryCreditNames> libraryCreditNames(const LibraryStore& store,
                                                             std::shared_ptr<const LibraryCreditNames> cached,
                                                             const CancellationToken& token = {});
// A credit's parts as pages show them; a part is marked as a name only when the Artists list holds it, so it opens that
// artist's albums.
std::vector<LibraryNamePart> creditParts(const LibraryCreditNames& names, std::string_view credit);

// The initials of browseGroups(field); none for years.
std::vector<LibraryInitial> groupInitials(const LibraryStore& store, LibraryGroupField field,
                                          const CancellationToken& token = {});
// The initials of browseAlbums(sort) without a filter; none for the year and date sorts.
std::vector<LibraryInitial> albumInitials(const LibraryStore& store, AlbumSort sort, const CancellationToken& token = {});
LibraryTrackRowPage browseTrackRows(const LibraryStore& store, TrackSort sort, bool descending,
                                    const std::optional<LibraryGroupFilter>& filter, int64_t offset, int64_t limit,
                                    std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
// `songs` of the tracks and at most `albums` of each album listing.
LibraryArtistPage artistPage(const LibraryStore& store, const LibraryGroupFilter& artist, int64_t songs, int64_t albums,
                             const CancellationToken& token = {});
LibraryTrackRowPage smartTracks(const LibraryStore& store, SmartList list, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
// Favorite albums, the latest marked first.
LibraryAlbumPage favoriteAlbums(const LibraryStore& store, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
// Favorite artists, then favorite composers, each the latest marked first; a name the library no longer holds is left
// out until it returns.
LibraryGroupPage favoriteArtists(const LibraryStore& store, int64_t offset, int64_t limit,
                                 std::optional<uint64_t> expectedRevision = std::nullopt, const CancellationToken& token = {});
// Marks or clears artists and composers (the filters' field and value) as favorites; a name already marked keeps the time
// it became one.
void setArtistFavorite(LibraryStore& store, const std::vector<LibraryGroupFilter>& artists, bool favorite);
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
