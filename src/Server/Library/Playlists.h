// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Library/PlayStatistics.h"

namespace camel::server {

// One selected source (docs/Plan.md §7.9). Folders include all subfolders; groups match a multivalued field value.
// A path names an available library file, such as a queue entry.
struct PlaylistSource {
    enum class Kind : uint8_t { Folder, Album, Track, Artist, Composer, Genre, Year, Path };
    Kind kind = Kind::Track;
    int64_t id = 0;        // folder or album id, or the year
    LibraryTrackKey track; // Track
    std::string value;     // Artist, Composer, Genre, or the file path
    bool operator==(const PlaylistSource&) const = default;
};

struct SelectionPreview {
    int64_t tracks = 0, durationMs = 0, unavailable = 0, duplicates = 0;
};

// A queue entry saved as is: a library track identity when it has one, and its source description either way.
struct PlaylistEntry {
    std::optional<LibraryTrackKey> track;
    std::string source;
};

// Lets the client undo a quick create or add while the playlist is unchanged since.
struct PlaylistUndo {
    int64_t playlist = 0, revision = 0;
    bool created = false;
    std::vector<int64_t> items;
};

struct PlaylistChange {
    int64_t playlist = 0, revision = 0;
    std::string name;
    int64_t added = 0, skippedUnavailable = 0, skippedDuplicates = 0, skippedExisting = 0;
    PlaylistUndo undo;
};

struct PlaylistSummary {
    int64_t id = 0, revision = 0, tracks = 0, created = 0, modified = 0;
    std::string name;
};

struct PlaylistItem {
    int64_t id = 0, position = 0, added = 0, durationMs = 0;
    std::optional<LibraryTrackKey> track;
    std::string source, title, artist, album;
    std::string path; // the file's current path, or the saved source for an entry outside the library
    std::string name; // the file's on-disk name, empty outside the library or before a scan records it
    bool available = false;
    bool favorite = false;
};

struct PlaylistPage {
    int64_t playlist = 0, revision = 0, total = 0;
    std::vector<PlaylistItem> items;
};

enum class PlaylistSortKey : uint8_t { Title, Artist, Album, AlbumArtist, Year, TrackNumber, Duration, DateAdded };

struct PlaylistEdit {
    enum class Kind : uint8_t { Rename, Delete, Remove, Move, Sort };
    Kind kind = Kind::Rename;
    int64_t playlist = 0;
    std::optional<int64_t> revision; // refuses the edit when the playlist changed since
    std::string name;                // Rename
    std::vector<int64_t> items;      // Remove, Move (kept in their current relative order)
    int64_t position = 0;            // Move: index of the first moved item among the remaining items
    PlaylistSortKey key = PlaylistSortKey::Title;
    bool descending = false;
};

SelectionPreview previewSelection(const LibraryStore& store, const std::vector<PlaylistSource>& sources,
                                  const CancellationToken& token = {});
// An empty name derives one from the first source ("first +N" for several sources); names are made unique with " (2)".
PlaylistChange createPlaylist(LibraryStore& store, std::string name, const std::vector<PlaylistSource>& sources,
                              const CancellationToken& token = {});
PlaylistChange addToPlaylist(LibraryStore& store, int64_t playlist, std::optional<int64_t> revision,
                             const std::vector<PlaylistSource>& sources, const CancellationToken& token = {});
// Keeps order, duplicates and entries without a library identity.
PlaylistChange savePlaylist(LibraryStore& store, std::string name, const std::vector<PlaylistEntry>& entries);
PlaylistChange editPlaylist(LibraryStore& store, const PlaylistEdit& edit);
// False when the playlist changed after the undoable action, which then stays as it is.
bool undoPlaylistChange(LibraryStore& store, const PlaylistUndo& undo);

// M3U8 files (docs/Plan.md §7.9). Import keeps the file's order and duplicates, resolves relative paths against its
// folder, keeps entries outside the library with their path only and skips URLs (counted as unavailable); a .m3u file
// that is not UTF-8 is read in the system code page. Export writes UTF-8 with #EXTINF lines, and paths relative to the
// file when they are on its drive or share.
PlaylistChange importPlaylistFile(LibraryStore& store, const std::filesystem::path& file);
void exportPlaylistFile(const LibraryStore& store, int64_t playlist, const std::filesystem::path& file);

std::vector<PlaylistSummary> listPlaylists(const LibraryStore& store);
PlaylistPage playlistItems(const LibraryStore& store, int64_t playlist, int64_t offset, int64_t limit);

} // namespace camel::server
