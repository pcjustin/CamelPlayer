// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibrarySchema.h"

#include <stdexcept>
#include <string>

namespace camel::server {
namespace {

// Identities: files.id is the file identity (kept across verified moves, docs/Plan.md §7.2); a track is
// (file_id, cue_index) with cue_index 0 for a whole file; queue entries have their own entry_id. Statistics and ratings
// are keyed by the track identity, not by tracks.id, so they outlive a track row that a rescan replaces.
constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE roots (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    online INTEGER NOT NULL DEFAULT 1,
    last_scan INTEGER,
    added INTEGER NOT NULL,
    removed INTEGER
);

CREATE TABLE folders (
    id INTEGER PRIMARY KEY,
    root_id INTEGER NOT NULL REFERENCES roots(id),
    parent_id INTEGER REFERENCES folders(id),
    path TEXT NOT NULL UNIQUE,
    mtime INTEGER
);

CREATE TABLE files (
    id INTEGER PRIMARY KEY,
    folder_id INTEGER REFERENCES folders(id),
    root_id INTEGER REFERENCES roots(id),
    path TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('audio', 'cue', 'image', 'lrc')),
    size INTEGER NOT NULL,
    mtime INTEGER NOT NULL,
    volume_serial INTEGER,
    file_index BLOB,
    content_hash BLOB,
    available INTEGER NOT NULL DEFAULT 1,
    last_seen INTEGER
);
CREATE UNIQUE INDEX files_available_path ON files(path) WHERE available = 1;
CREATE INDEX files_volume_index ON files(volume_serial, file_index);
CREATE INDEX files_folder ON files(folder_id);

CREATE TABLE covers (
    id INTEGER PRIMARY KEY,
    source TEXT NOT NULL,
    file_id INTEGER REFERENCES files(id),
    digest BLOB,
    mime TEXT,
    width INTEGER,
    height INTEGER,
    cache_path TEXT
);

CREATE TABLE albums (
    id INTEGER PRIMARY KEY,
    folder_id INTEGER REFERENCES folders(id),
    grouping_key TEXT NOT NULL UNIQUE,
    title TEXT NOT NULL,
    album_artist TEXT,
    year INTEGER,
    disc_count INTEGER,
    cover_id INTEGER REFERENCES covers(id),
    added INTEGER NOT NULL,
    title_norm TEXT,
    artist_norm TEXT,
    sort_key BLOB
);

CREATE TABLE tracks (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES files(id),
    cue_index INTEGER NOT NULL DEFAULT 0,
    cue_file_id INTEGER REFERENCES files(id),
    start_frame INTEGER NOT NULL DEFAULT 0,
    end_frame INTEGER,
    album_id INTEGER REFERENCES albums(id),
    title TEXT,
    album TEXT,
    album_artist TEXT,
    artist TEXT,
    composer TEXT,
    genre TEXT,
    year INTEGER,
    track_number INTEGER,
    track_total INTEGER,
    disc_number INTEGER,
    disc_total INTEGER,
    format TEXT,
    sample_rate INTEGER,
    bits_per_sample INTEGER,
    channels INTEGER,
    total_frames INTEGER,
    rg_track_gain REAL,
    rg_track_peak REAL,
    rg_album_gain REAL,
    rg_album_peak REAL,
    rg_analysis_gain REAL,
    rg_analysis_peak REAL,
    embedded_cover INTEGER NOT NULL DEFAULT 0,
    lyrics TEXT NOT NULL DEFAULT 'none' CHECK (lyrics IN ('none', 'embedded', 'lrc')),
    title_norm TEXT,
    album_norm TEXT,
    artist_norm TEXT,
    title_sort BLOB,
    album_sort BLOB,
    artist_sort BLOB,
    UNIQUE (file_id, cue_index)
);
CREATE INDEX tracks_album ON tracks(album_id);

CREATE TABLE track_values (
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    field TEXT NOT NULL CHECK (field IN ('artist', 'composer', 'genre')),
    position INTEGER NOT NULL,
    value TEXT NOT NULL,
    value_norm TEXT,
    sort_key BLOB,
    PRIMARY KEY (track_id, field, position)
);
CREATE INDEX track_values_lookup ON track_values(field, value_norm);

CREATE TABLE playlists (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    created INTEGER NOT NULL,
    modified INTEGER NOT NULL
);

CREATE TABLE playlist_items (
    playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    file_id INTEGER REFERENCES files(id),
    cue_index INTEGER NOT NULL DEFAULT 0,
    source_description TEXT NOT NULL,
    PRIMARY KEY (playlist_id, position)
);

CREATE TABLE queue (
    position INTEGER PRIMARY KEY,
    entry_id INTEGER NOT NULL UNIQUE,
    file_id INTEGER REFERENCES files(id),
    cue_index INTEGER NOT NULL DEFAULT 0,
    path TEXT NOT NULL,
    start_frame INTEGER NOT NULL DEFAULT 0,
    end_frame INTEGER,
    decoder TEXT,
    source_description TEXT NOT NULL
);

CREATE TABLE app_state (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE stats (
    file_id INTEGER NOT NULL REFERENCES files(id),
    cue_index INTEGER NOT NULL DEFAULT 0,
    play_count INTEGER NOT NULL DEFAULT 0,
    last_played INTEGER,
    rating INTEGER CHECK (rating BETWEEN 1 AND 5),
    favorite INTEGER NOT NULL DEFAULT 0,
    detached INTEGER,
    PRIMARY KEY (file_id, cue_index)
);

CREATE TABLE change_log (
    id INTEGER PRIMARY KEY,
    time INTEGER NOT NULL,
    action TEXT NOT NULL,
    old_path TEXT,
    new_path TEXT,
    before_json TEXT,
    after_json TEXT,
    undo_state TEXT NOT NULL DEFAULT 'none',
    journal_ref TEXT NOT NULL
);

CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title_norm, album_norm, artist_norm,
    content = 'tracks', content_rowid = 'id', tokenize = 'trigram'
);
)sql";

} // namespace

const std::vector<Migration>& libraryMigrations() {
    static const std::vector<Migration> migrations = {
        [](sqlite::Database& db) { db.exec(kSchemaV1); },
        [](sqlite::Database& db) {
            // Identity state: established (0), awaiting a complete scan (1), superseded provisional record (2).
            db.exec("ALTER TABLE files ADD COLUMN created INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE files ADD COLUMN volume_key TEXT;"
                    "ALTER TABLE files ADD COLUMN identity_state INTEGER NOT NULL DEFAULT 0;"
                    "CREATE INDEX files_trusted_identity ON files(volume_key,file_index,created);"
                    "CREATE INDEX files_content_hash ON files(content_hash) WHERE content_hash IS NOT NULL;"
                    "CREATE INDEX files_unfingerprinted_size ON files(kind,size) "
                    "WHERE content_hash IS NULL AND identity_state<>2;");
        },
        [](sqlite::Database& db) {
            db.exec("ALTER TABLE tracks ADD COLUMN cue_pregap_75ths INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE tracks ADD COLUMN cue_postgap_75ths INTEGER NOT NULL DEFAULT 0;"
                    "CREATE VIEW library_tracks AS SELECT t.* FROM tracks t WHERE t.cue_index>0 OR "
                    "(t.cue_index=0 AND NOT EXISTS(SELECT 1 FROM tracks v WHERE v.file_id=t.file_id AND v.cue_index<>0));");
        },
        [](sqlite::Database& db) {
            db.exec("ALTER TABLE tracks ADD COLUMN title_sort_tag TEXT;"
                    "ALTER TABLE tracks ADD COLUMN album_sort_tag TEXT;"
                    "ALTER TABLE tracks ADD COLUMN artist_sort_tag TEXT;"
                    "ALTER TABLE track_values ADD COLUMN sort_source TEXT;"
                    "CREATE INDEX tracks_title_sort ON tracks(title_sort,id);"
                    "CREATE INDEX tracks_artist_sort ON tracks(artist_sort,id);"
                    "CREATE INDEX tracks_album_sort ON tracks(album_sort,id);"
                    "CREATE INDEX albums_sort ON albums(sort_key,id);"
                    "CREATE INDEX track_values_sort ON track_values(field,sort_key);"
                    "UPDATE tracks SET title_norm=coalesce(title_norm,title,''),album_norm=coalesce(album_norm,album,''),"
                    "artist_norm=coalesce(artist_norm,artist,'');"
                    "CREATE TABLE library_revision (id INTEGER PRIMARY KEY CHECK(id=1), value INTEGER NOT NULL);"
                    "INSERT INTO library_revision VALUES(1,0);"
                    "CREATE TRIGGER tracks_revision_insert AFTER INSERT ON tracks BEGIN UPDATE library_revision SET value=value+1 WHERE id=1; END;"
                    "CREATE TRIGGER tracks_revision_update AFTER UPDATE ON tracks BEGIN UPDATE library_revision SET value=value+1 WHERE id=1; END;"
                    "CREATE TRIGGER tracks_revision_delete AFTER DELETE ON tracks BEGIN UPDATE library_revision SET value=value+1 WHERE id=1; END;"
                    "CREATE TRIGGER files_revision_update AFTER UPDATE OF available ON files BEGIN UPDATE library_revision SET value=value+1 WHERE id=1; END;"
                    "CREATE TRIGGER roots_revision_update AFTER UPDATE OF online ON roots BEGIN UPDATE library_revision SET value=value+1 WHERE id=1; END;"
                    "CREATE TRIGGER tracks_fts_insert AFTER INSERT ON tracks BEGIN INSERT INTO tracks_fts(rowid,title_norm,album_norm,artist_norm) "
                    "VALUES(new.id,coalesce(new.title_norm,new.title,''),coalesce(new.album_norm,new.album,''),coalesce(new.artist_norm,new.artist,'')); END;"
                    "CREATE TRIGGER tracks_fts_update AFTER UPDATE ON tracks "
                    "WHEN old.title IS NOT new.title OR old.album IS NOT new.album OR old.artist IS NOT new.artist "
                    "OR old.title_norm IS NOT new.title_norm OR old.album_norm IS NOT new.album_norm OR old.artist_norm IS NOT new.artist_norm "
                    "BEGIN INSERT INTO tracks_fts(tracks_fts,rowid,title_norm,album_norm,artist_norm) "
                    "VALUES('delete',old.id,coalesce(old.title_norm,old.title,''),coalesce(old.album_norm,old.album,''),coalesce(old.artist_norm,old.artist,''));"
                    "INSERT INTO tracks_fts(rowid,title_norm,album_norm,artist_norm) VALUES(new.id,coalesce(new.title_norm,new.title,''),"
                    "coalesce(new.album_norm,new.album,''),coalesce(new.artist_norm,new.artist,'')); END;"
                    "CREATE TRIGGER tracks_fts_delete AFTER DELETE ON tracks BEGIN INSERT INTO tracks_fts(tracks_fts,rowid,title_norm,album_norm,artist_norm) "
                    "VALUES('delete',old.id,coalesce(old.title_norm,old.title,''),coalesce(old.album_norm,old.album,''),coalesce(old.artist_norm,old.artist,'')); END;"
                    "INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild');");
        },
        [](sqlite::Database& db) {
            // An estimated total_frames is replaced by the exact length the first time playback opens the file.
            db.exec("ALTER TABLE tracks ADD COLUMN length_estimated INTEGER NOT NULL DEFAULT 0;");
        },
        [](sqlite::Database& db) {
            // One row per play session (started: UTC ms; utc_offset: local minutes east of UTC); kept permanently.
            db.exec("ALTER TABLE stats ADD COLUMN listened_ms INTEGER NOT NULL DEFAULT 0;"
                    "CREATE TABLE play_events (id INTEGER PRIMARY KEY, file_id INTEGER NOT NULL REFERENCES files(id),"
                    "cue_index INTEGER NOT NULL DEFAULT 0, started INTEGER NOT NULL, utc_offset INTEGER NOT NULL,"
                    "heard_ms INTEGER NOT NULL, counted INTEGER NOT NULL DEFAULT 0);"
                    "CREATE INDEX play_events_started ON play_events(started);"
                    "CREATE INDEX play_events_track ON play_events(file_id,cue_index);");
        },
        [](sqlite::Database& db) {
            // Items get stable ids for quick Undo and an added time for sorting; positions are renumbered on edits.
            db.exec("ALTER TABLE playlists ADD COLUMN revision INTEGER NOT NULL DEFAULT 0;"
                    "CREATE UNIQUE INDEX playlists_name ON playlists(name);"
                    "CREATE TABLE playlist_items_v7 (id INTEGER PRIMARY KEY,"
                    "playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
                    "position INTEGER NOT NULL, file_id INTEGER REFERENCES files(id), cue_index INTEGER NOT NULL DEFAULT 0,"
                    "source_description TEXT NOT NULL, added INTEGER NOT NULL DEFAULT 0);"
                    "INSERT INTO playlist_items_v7(playlist_id,position,file_id,cue_index,source_description) "
                    "SELECT playlist_id,position,file_id,cue_index,source_description FROM playlist_items;"
                    "DROP TABLE playlist_items;"
                    "ALTER TABLE playlist_items_v7 RENAME TO playlist_items;"
                    "CREATE INDEX playlist_items_order ON playlist_items(playlist_id,position);"
                    "CREATE INDEX playlist_items_track ON playlist_items(file_id,cue_index);");
        },
    };
    return migrations;
}

void migrate(sqlite::Database& db, const std::vector<Migration>& migrations) {
    const int current = db.userVersion();
    if (current > int(migrations.size()))
        throw std::runtime_error("library.db has schema version " + std::to_string(current) + ", newer than this program (" +
                                 std::to_string(migrations.size()) + ")");
    for (int version = current; version < int(migrations.size()); version++) {
        sqlite::Transaction t(db);
        migrations[size_t(version)](db);
        db.setUserVersion(version + 1);
        t.commit();
    }
}

} // namespace camel::server
