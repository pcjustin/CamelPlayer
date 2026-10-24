// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/Playlists.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <sqlite3.h>

#include "Server/Library/LibraryViews.h"

namespace camel::server {
namespace {

constexpr const char* kTrackColumns =
    "t.file_id,t.cue_index,(f.available=1 AND r.online=1 AND r.removed IS NULL),"
    "CASE WHEN t.sample_rate>0 THEN (coalesce(t.end_frame,t.total_frames,0)-t.start_frame)*1000/t.sample_rate ELSE 0 END,"
    "f.path ";
constexpr const char* kTrackJoins = " JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id "
                                    "LEFT JOIN albums al ON al.id=t.album_id ";
// Album, then disc and track number; the file name orders tracks without numbers and CUE index orders virtual tracks.
constexpr const char* kNaturalOrder =
    " ORDER BY coalesce(al.sort_key,t.album_sort),t.album_id,t.disc_number,t.track_number,f.path,t.cue_index";

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Resolved {
    LibraryTrackKey track;
    std::string source;
    int64_t durationMs = 0;
};

struct Expansion {
    std::vector<Resolved> tracks;
    int64_t unavailable = 0, duplicates = 0;
    std::string firstName;
};

std::string sourceName(sqlite::Database& db, const PlaylistSource& source) {
    switch (source.kind) {
    case PlaylistSource::Kind::Folder: {
        auto row = db.prepare("SELECT path FROM folders WHERE id=?");
        return row.bind(1, source.id).step() ? libraryDisplayName(row.text(0)) : std::string{};
    }
    case PlaylistSource::Kind::Album: {
        auto row = db.prepare("SELECT title FROM albums WHERE id=?");
        return row.bind(1, source.id).step() ? row.text(0) : std::string{};
    }
    case PlaylistSource::Kind::Track: {
        auto row = db.prepare("SELECT coalesce(title,'') FROM tracks WHERE file_id=? AND cue_index=?");
        return row.bind(1, source.track.file).bind(2, source.track.cueIndex).step() ? row.text(0) : std::string{};
    }
    default:
        return source.value;
    }
}

Expansion expand(sqlite::Database& db, const std::vector<PlaylistSource>& sources, const CancellationToken& token) {
    sqlite3_progress_handler(db.handle(), 1000, [](void* context) {
        return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0;
    }, const_cast<CancellationToken*>(&token));
    Expansion out;
    std::set<std::pair<int64_t, int64_t>> seen;
    const std::string select = std::string("SELECT ") + kTrackColumns;
    for (const auto& source : sources) {
        if (token.cancelled()) throw std::runtime_error("playlist selection cancelled");
        sqlite::Statement rows = [&] {
            switch (source.kind) {
            case PlaylistSource::Kind::Folder: {
                auto s = db.prepare("WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL SELECT d.id FROM folders d JOIN sub "
                                    "ON d.parent_id=sub.id) " + select + "FROM library_tracks t" + kTrackJoins +
                                    "WHERE f.folder_id IN sub" + kNaturalOrder);
                s.bind(1, source.id);
                return s;
            }
            case PlaylistSource::Kind::Album: {
                auto s = db.prepare(select + "FROM library_tracks t" + kTrackJoins + "WHERE t.album_id=?" + kNaturalOrder);
                s.bind(1, source.id);
                return s;
            }
            case PlaylistSource::Kind::Track: {
                auto s = db.prepare(select + "FROM tracks t" + kTrackJoins + "WHERE t.file_id=? AND t.cue_index=?");
                s.bind(1, source.track.file).bind(2, source.track.cueIndex);
                return s;
            }
            default: {
                const char* field = source.kind == PlaylistSource::Kind::Artist     ? "artist"
                                    : source.kind == PlaylistSource::Kind::Composer ? "composer"
                                                                                    : "genre";
                // Tracks without multivalue rows match their single-valued column.
                auto s = db.prepare(select + "FROM library_tracks t" + kTrackJoins +
                                    "WHERE EXISTS(SELECT 1 FROM track_values v WHERE v.track_id=t.id AND v.field=?1 AND "
                                    "v.value=?2) OR (NOT EXISTS(SELECT 1 FROM track_values v WHERE v.track_id=t.id AND "
                                    "v.field=?1) AND t." + field + "=?2)" + kNaturalOrder);
                s.bind(1, std::string_view(field)).bind(2, source.value);
                return s;
            }
            }
        }();
        if (out.firstName.empty()) out.firstName = sourceName(db, source);
        while (rows.step()) {
            const LibraryTrackKey track{rows.integer(0), rows.integer(1)};
            if (!rows.integer(2)) {
                ++out.unavailable;
            } else if (!seen.emplace(track.file, track.cueIndex).second) {
                ++out.duplicates;
            } else {
                out.tracks.push_back({track, rows.text(4), rows.integer(3)});
            }
        }
    }
    return out;
}

std::string uniqueName(sqlite::Database& db, const std::string& base, int64_t except = 0) {
    auto taken = db.prepare("SELECT 1 FROM playlists WHERE name=? AND id<>?");
    for (int n = 1;; ++n) {
        const auto candidate = n == 1 ? base : base + " (" + std::to_string(n) + ")";
        taken.reset();
        if (!taken.bind(1, candidate).bind(2, except).step()) return candidate;
    }
}

int64_t insertPlaylist(sqlite::Database& db, const std::string& base) {
    const auto now = nowMs();
    db.prepare("INSERT INTO playlists(name,created,modified,revision) VALUES(?,?,?,1)")
        .bind(1, uniqueName(db, base.empty() ? std::string("Playlist") : base))
        .bind(2, now)
        .bind(3, now)
        .run();
    return db.lastInsertId();
}

// Appends after the last item and returns the new item ids.
std::vector<int64_t> appendItems(sqlite::Database& db, int64_t playlist, const std::vector<PlaylistEntry>& entries) {
    int64_t position = db.scalar("SELECT coalesce(MAX(position)+1,0) FROM playlist_items WHERE playlist_id=" +
                                 std::to_string(playlist)).value();
    auto insert = db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,cue_index,source_description,added) "
                             "VALUES(?,?,?,?,?,?)");
    const auto now = nowMs();
    std::vector<int64_t> ids;
    ids.reserve(entries.size());
    for (const auto& entry : entries) {
        insert.reset();
        insert.bind(1, playlist).bind(2, position++);
        if (entry.track) insert.bind(3, entry.track->file).bind(4, entry.track->cueIndex);
        else insert.bindNull(3).bind(4, int64_t(0));
        insert.bind(5, entry.source).bind(6, now).run();
        ids.push_back(db.lastInsertId());
    }
    return ids;
}

void renumber(sqlite::Database& db, const std::vector<int64_t>& ordered) {
    auto update = db.prepare("UPDATE playlist_items SET position=? WHERE id=?");
    for (size_t i = 0; i < ordered.size(); ++i) {
        update.reset();
        update.bind(1, int64_t(i)).bind(2, ordered[i]).run();
    }
}

std::vector<int64_t> orderedItems(sqlite::Database& db, int64_t playlist) {
    std::vector<int64_t> ids;
    auto rows = db.prepare("SELECT id FROM playlist_items WHERE playlist_id=? ORDER BY position");
    rows.bind(1, playlist);
    while (rows.step()) ids.push_back(rows.integer(0));
    return ids;
}

// Checks the expected revision and returns the next one, which the caller's change records.
int64_t bump(sqlite::Database& db, int64_t playlist, std::optional<int64_t> expected) {
    auto row = db.prepare("SELECT revision FROM playlists WHERE id=?");
    if (!row.bind(1, playlist).step()) throw std::invalid_argument("unknown playlist");
    if (expected && *expected != row.integer(0)) throw std::runtime_error("playlist changed since it was read");
    const int64_t revision = row.integer(0) + 1;
    db.prepare("UPDATE playlists SET revision=?,modified=? WHERE id=?").bind(1, revision).bind(2, nowMs()).bind(3, playlist).run();
    return revision;
}

std::string playlistName(sqlite::Database& db, int64_t playlist) {
    auto row = db.prepare("SELECT name FROM playlists WHERE id=?");
    return row.bind(1, playlist).step() ? row.text(0) : std::string{};
}

std::vector<PlaylistEntry> entries(const std::vector<Resolved>& tracks) {
    std::vector<PlaylistEntry> out;
    out.reserve(tracks.size());
    for (const auto& t : tracks) out.push_back({t.track, t.source});
    return out;
}

std::string defaultName(const Expansion& expansion, size_t sources) {
    if (sources <= 1) return expansion.firstName;
    return expansion.firstName + " +" + std::to_string(sources - 1);
}

} // namespace

SelectionPreview previewSelection(const LibraryStore& store, const std::vector<PlaylistSource>& sources,
                                  const CancellationToken& token) {
    auto db = store.openReader();
    db.exec("BEGIN");
    const auto expansion = expand(db, sources, token);
    db.exec("COMMIT");
    SelectionPreview out{int64_t(expansion.tracks.size()), 0, expansion.unavailable, expansion.duplicates};
    for (const auto& t : expansion.tracks) out.durationMs += t.durationMs;
    return out;
}

PlaylistChange createPlaylist(LibraryStore& store, std::string name, const std::vector<PlaylistSource>& sources,
                              const CancellationToken& token) {
    if (sources.empty()) throw std::invalid_argument("a playlist needs a selection");
    auto reader = store.openReader();
    reader.exec("BEGIN");
    const auto expansion = expand(reader, sources, token);
    reader.exec("COMMIT");
    if (name.empty()) name = defaultName(expansion, sources.size());
    PlaylistChange change;
    store
        .write([&](sqlite::Database& db) {
            change.playlist = insertPlaylist(db, name);
            change.undo.items = appendItems(db, change.playlist, entries(expansion.tracks));
            change.name = playlistName(db, change.playlist);
            change.revision = 1;
        })
        .get();
    change.added = int64_t(expansion.tracks.size());
    change.skippedUnavailable = expansion.unavailable;
    change.skippedDuplicates = expansion.duplicates;
    change.undo = {change.playlist, change.revision, true, std::move(change.undo.items)};
    return change;
}

PlaylistChange addToPlaylist(LibraryStore& store, int64_t playlist, std::optional<int64_t> revision,
                             const std::vector<PlaylistSource>& sources, const CancellationToken& token) {
    if (sources.empty()) throw std::invalid_argument("nothing to add");
    auto reader = store.openReader();
    reader.exec("BEGIN");
    const auto expansion = expand(reader, sources, token);
    reader.exec("COMMIT");
    PlaylistChange change;
    change.playlist = playlist;
    change.skippedUnavailable = expansion.unavailable;
    change.skippedDuplicates = expansion.duplicates;
    store
        .write([&](sqlite::Database& db) {
            change.revision = bump(db, playlist, revision);
            std::set<std::pair<int64_t, int64_t>> present;
            auto existing = db.prepare("SELECT file_id,cue_index FROM playlist_items WHERE playlist_id=? AND file_id IS NOT NULL");
            existing.bind(1, playlist);
            while (existing.step()) present.emplace(existing.integer(0), existing.integer(1));
            std::vector<PlaylistEntry> added;
            for (const auto& t : expansion.tracks) {
                if (present.contains({t.track.file, t.track.cueIndex})) ++change.skippedExisting;
                else added.push_back({t.track, t.source});
            }
            change.undo.items = appendItems(db, playlist, added);
            change.name = playlistName(db, playlist);
        })
        .get();
    change.added = int64_t(change.undo.items.size());
    change.undo.playlist = playlist;
    change.undo.revision = change.revision;
    return change;
}

PlaylistChange savePlaylist(LibraryStore& store, std::string name, const std::vector<PlaylistEntry>& items) {
    PlaylistChange change;
    store
        .write([&](sqlite::Database& db) {
            change.playlist = insertPlaylist(db, name);
            change.undo.items = appendItems(db, change.playlist, items);
            change.name = playlistName(db, change.playlist);
            change.revision = 1;
        })
        .get();
    change.added = int64_t(items.size());
    change.undo = {change.playlist, change.revision, true, std::move(change.undo.items)};
    return change;
}

PlaylistChange editPlaylist(LibraryStore& store, const PlaylistEdit& edit) {
    PlaylistChange change;
    change.playlist = edit.playlist;
    store
        .write([&](sqlite::Database& db) {
            change.revision = bump(db, edit.playlist, edit.revision);
            switch (edit.kind) {
            case PlaylistEdit::Kind::Rename:
                if (edit.name.empty()) throw std::invalid_argument("a playlist needs a name");
                db.prepare("UPDATE playlists SET name=? WHERE id=?")
                    .bind(1, uniqueName(db, edit.name, edit.playlist))
                    .bind(2, edit.playlist)
                    .run();
                break;
            case PlaylistEdit::Kind::Delete:
                db.prepare("DELETE FROM playlists WHERE id=?").bind(1, edit.playlist).run();
                change.revision = 0;
                return;
            case PlaylistEdit::Kind::Remove: {
                auto remove = db.prepare("DELETE FROM playlist_items WHERE id=? AND playlist_id=?");
                for (const auto id : edit.items) {
                    remove.reset();
                    remove.bind(1, id).bind(2, edit.playlist).run();
                }
                renumber(db, orderedItems(db, edit.playlist));
                break;
            }
            case PlaylistEdit::Kind::Move: {
                const std::set<int64_t> moving(edit.items.begin(), edit.items.end());
                std::vector<int64_t> rest, moved;
                for (const auto id : orderedItems(db, edit.playlist)) (moving.contains(id) ? moved : rest).push_back(id);
                if (moved.size() != moving.size()) throw std::invalid_argument("item is not in this playlist");
                const auto at = rest.begin() + std::clamp<int64_t>(edit.position, 0, int64_t(rest.size()));
                rest.insert(at, moved.begin(), moved.end());
                renumber(db, rest);
                break;
            }
            case PlaylistEdit::Kind::Sort: {
                static const std::vector<std::vector<const char*>> keys = {
                    {"lower(t.title)"}, {"lower(t.artist)"}, {"lower(t.album)"}, {"lower(t.album_artist)"}, {"t.year"},
                    {"t.disc_number", "t.track_number"},
                    {"CASE WHEN t.sample_rate>0 THEN (coalesce(t.end_frame,t.total_frames,0)-t.start_frame)*1000/t.sample_rate END"},
                    {"i.added"}};
                // Entries without library metadata sort last; equal keys keep their current order.
                std::string order = "(t.id IS NULL)";
                for (const auto* column : keys[size_t(edit.key)]) order += std::string(",") + column + (edit.descending ? " DESC" : "");
                auto rows = db.prepare("SELECT i.id FROM playlist_items i LEFT JOIN tracks t ON t.file_id=i.file_id AND "
                                       "t.cue_index=i.cue_index WHERE i.playlist_id=? ORDER BY " + order + ",i.position");
                rows.bind(1, edit.playlist);
                std::vector<int64_t> ordered;
                while (rows.step()) ordered.push_back(rows.integer(0));
                renumber(db, ordered);
                break;
            }
            }
            change.name = playlistName(db, edit.playlist);
        })
        .get();
    return change;
}

bool undoPlaylistChange(LibraryStore& store, const PlaylistUndo& undo) {
    bool undone = false;
    store
        .write([&](sqlite::Database& db) {
            auto row = db.prepare("SELECT revision FROM playlists WHERE id=?");
            if (!row.bind(1, undo.playlist).step() || row.integer(0) != undo.revision) return;
            if (undo.created) {
                db.prepare("DELETE FROM playlists WHERE id=?").bind(1, undo.playlist).run();
            } else {
                auto remove = db.prepare("DELETE FROM playlist_items WHERE id=? AND playlist_id=?");
                for (const auto id : undo.items) {
                    remove.reset();
                    remove.bind(1, id).bind(2, undo.playlist).run();
                }
                renumber(db, orderedItems(db, undo.playlist));
                bump(db, undo.playlist, undo.revision);
            }
            undone = true;
        })
        .get();
    return undone;
}

std::vector<PlaylistSummary> listPlaylists(const LibraryStore& store) {
    auto db = store.openReader();
    auto rows = db.prepare("SELECT p.id,p.revision,(SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id=p.id),"
                           "p.created,p.modified,p.name FROM playlists p ORDER BY p.name,p.id");
    std::vector<PlaylistSummary> out;
    while (rows.step())
        out.push_back({rows.integer(0), rows.integer(1), rows.integer(2), rows.integer(3), rows.integer(4), rows.text(5)});
    return out;
}

PlaylistPage playlistItems(const LibraryStore& store, int64_t playlist, int64_t offset, int64_t limit) {
    if (offset < 0 || limit < 1 || limit > 500) throw std::invalid_argument("invalid playlist page range");
    auto db = store.openReader();
    db.exec("BEGIN");
    PlaylistPage page;
    page.playlist = playlist;
    auto header = db.prepare("SELECT revision,(SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1) FROM playlists WHERE id=?1");
    if (!header.bind(1, playlist).step()) throw std::invalid_argument("unknown playlist");
    page.revision = header.integer(0);
    page.total = header.integer(1);
    auto rows = db.prepare(
        "SELECT i.id,i.position,i.added,i.file_id,i.cue_index,i.source_description,coalesce(t.title,''),"
        "coalesce(t.artist,''),coalesce(t.album,''),"
        "CASE WHEN t.sample_rate>0 THEN (coalesce(t.end_frame,t.total_frames,0)-t.start_frame)*1000/t.sample_rate ELSE 0 END,"
        "coalesce(t.id IS NOT NULL AND f.available=1 AND r.online=1 AND r.removed IS NULL,0) "
        "FROM playlist_items i LEFT JOIN tracks t ON t.file_id=i.file_id AND t.cue_index=i.cue_index "
        "LEFT JOIN files f ON f.id=i.file_id LEFT JOIN roots r ON r.id=f.root_id "
        "WHERE i.playlist_id=? ORDER BY i.position LIMIT ? OFFSET ?");
    rows.bind(1, playlist).bind(2, limit).bind(3, offset);
    while (rows.step()) {
        PlaylistItem item;
        item.id = rows.integer(0);
        item.position = rows.integer(1);
        item.added = rows.integer(2);
        if (!rows.isNull(3)) item.track = LibraryTrackKey{rows.integer(3), rows.integer(4)};
        item.source = rows.text(5);
        item.title = rows.text(6);
        item.artist = rows.text(7);
        item.album = rows.text(8);
        item.durationMs = rows.integer(9);
        item.available = rows.integer(10) != 0;
        page.items.push_back(std::move(item));
    }
    db.exec("COMMIT");
    return page;
}

} // namespace camel::server
