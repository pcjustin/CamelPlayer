// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// JSON for the library, playlist and other types of the Server API's calls (docs/Plan.md §17.8, docs/Server-API.md).
// A missing field, a value of the wrong type and an unknown enum name are refused with an exception.
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

#include "Server/Api/ServerApi.h"

// An enum as its names; a name it does not have is refused.
#define CAMEL_JSON_ENUM(E, ...)                                                                                                  \
    inline const std::pair<E, const char*>* camelJsonNames(E /*tag*/, size_t& count) {                                           \
        static const std::pair<E, const char*> names[] = __VA_ARGS__;                                                            \
        count = std::size(names);                                                                                                \
        return names;                                                                                                            \
    }                                                                                                                            \
    inline void to_json(nlohmann::json& j, const E& e) {                                                                         \
        size_t count = 0;                                                                                                        \
        const auto* names = camelJsonNames(e, count);                                                                            \
        for (size_t i = 0; i < count; ++i)                                                                                       \
            if (names[i].first == e) {                                                                                           \
                j = names[i].second;                                                                                             \
                return;                                                                                                          \
            }                                                                                                                    \
        throw std::invalid_argument("API JSON: a " #E " without a name");                                                        \
    }                                                                                                                            \
    inline void from_json(const nlohmann::json& j, E& e) {                                                                       \
        size_t count = 0;                                                                                                        \
        const auto* names = camelJsonNames(e, count);                                                                            \
        if (j.is_string())                                                                                                       \
            for (size_t i = 0; i < count; ++i)                                                                                   \
                if (j.get_ref<const std::string&>() == names[i].second) {                                                        \
                    e = names[i].first;                                                                                          \
                    return;                                                                                                      \
                }                                                                                                                \
        throw std::invalid_argument("API JSON: not a " #E);                                                                      \
    }

// A struct as its fields, every one needed when it is read.
#define CAMEL_JSON_STRUCT(Type, ...)                                                                                             \
    inline void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) {                                          \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__))                                                 \
    }                                                                                                                            \
    inline void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) {                                        \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, __VA_ARGS__))                                               \
    }

namespace camel {

CAMEL_JSON_ENUM(SettingsRecovery, {{SettingsRecovery::None, "none"},
                                   {SettingsRecovery::RestoredLastGood, "restoredLastGood"},
                                   {SettingsRecovery::Reset, "reset"}})

} // namespace camel

namespace camel::server {

CAMEL_JSON_ENUM(LibraryGroupField, {{LibraryGroupField::Artist, "artist"},
                                    {LibraryGroupField::Composer, "composer"},
                                    {LibraryGroupField::Genre, "genre"},
                                    {LibraryGroupField::Year, "year"}})
CAMEL_JSON_ENUM(AlbumSort, {{AlbumSort::Title, "title"},
                            {AlbumSort::Artist, "artist"},
                            {AlbumSort::Year, "year"},
                            {AlbumSort::Added, "added"}})
CAMEL_JSON_ENUM(TrackSort, {{TrackSort::Title, "title"},
                            {TrackSort::Artist, "artist"},
                            {TrackSort::Album, "album"},
                            {TrackSort::Year, "year"},
                            {TrackSort::Duration, "duration"},
                            {TrackSort::Added, "added"}})
CAMEL_JSON_ENUM(SmartList, {{SmartList::RecentlyAdded, "recentlyAdded"},
                            {SmartList::MostPlayed, "mostPlayed"},
                            {SmartList::RecentlyPlayed, "recentlyPlayed"},
                            {SmartList::Favorites, "favorites"},
                            {SmartList::NeverPlayed, "neverPlayed"}})
CAMEL_JSON_ENUM(LibraryAlbumRole,
                {{LibraryAlbumRole::Any, "any"}, {LibraryAlbumRole::Own, "own"}, {LibraryAlbumRole::Guest, "guest"}})
CAMEL_JSON_ENUM(PlayMetric, {{PlayMetric::Plays, "plays"}, {PlayMetric::ListeningTime, "listeningTime"}})
CAMEL_JSON_ENUM(PlayBucketSize, {{PlayBucketSize::Day, "day"}, {PlayBucketSize::Week, "week"}, {PlayBucketSize::Month, "month"}})
CAMEL_JSON_ENUM(ArtworkKind, {{ArtworkKind::Album, "album"},
                              {ArtworkKind::Folder, "folder"},
                              {ArtworkKind::Track, "track"},
                              {ArtworkKind::Artist, "artist"}})
CAMEL_JSON_ENUM(PlaylistSource::Kind, {{PlaylistSource::Kind::Folder, "folder"},
                                       {PlaylistSource::Kind::Album, "album"},
                                       {PlaylistSource::Kind::Track, "track"},
                                       {PlaylistSource::Kind::Artist, "artist"},
                                       {PlaylistSource::Kind::Composer, "composer"},
                                       {PlaylistSource::Kind::Genre, "genre"},
                                       {PlaylistSource::Kind::Year, "year"},
                                       {PlaylistSource::Kind::Path, "path"}})
CAMEL_JSON_ENUM(PlaylistSortKey, {{PlaylistSortKey::Title, "title"},
                                  {PlaylistSortKey::Artist, "artist"},
                                  {PlaylistSortKey::Album, "album"},
                                  {PlaylistSortKey::AlbumArtist, "albumArtist"},
                                  {PlaylistSortKey::Year, "year"},
                                  {PlaylistSortKey::TrackNumber, "trackNumber"},
                                  {PlaylistSortKey::Duration, "duration"},
                                  {PlaylistSortKey::DateAdded, "dateAdded"}})
CAMEL_JSON_ENUM(PlaylistEdit::Kind, {{PlaylistEdit::Kind::Rename, "rename"},
                                     {PlaylistEdit::Kind::Delete, "delete"},
                                     {PlaylistEdit::Kind::Remove, "remove"},
                                     {PlaylistEdit::Kind::Move, "move"},
                                     {PlaylistEdit::Kind::Sort, "sort"}})
CAMEL_JSON_ENUM(LibraryScanStart, {{LibraryScanStart::Started, "started"},
                                   {LibraryScanStart::Running, "running"},
                                   {LibraryScanStart::Offline, "offline"}})
CAMEL_JSON_ENUM(FileDetails::Problem, {{FileDetails::Problem::None, "none"},
                                       {FileDetails::Problem::Missing, "missing"},
                                       {FileDetails::Problem::Unreadable, "unreadable"}})
CAMEL_JSON_ENUM(DataRecovery::Library, {{DataRecovery::Library::None, "none"},
                                        {DataRecovery::Library::RestoredBackup, "restoredBackup"},
                                        {DataRecovery::Library::Rebuilt, "rebuilt"}})
CAMEL_JSON_ENUM(DiscordConnection, {{DiscordConnection::Off, "off"},
                                    {DiscordConnection::Searching, "searching"},
                                    {DiscordConnection::Connected, "connected"}})
CAMEL_JSON_ENUM(DiscordCover, {{DiscordCover::Off, "off"},
                               {DiscordCover::Downloading, "downloading"},
                               {DiscordCover::Starting, "starting"},
                               {DiscordCover::Ready, "ready"},
                               {DiscordCover::Failed, "failed"}})

CAMEL_JSON_STRUCT(LibraryTrackKey, file)
CAMEL_JSON_STRUCT(LibraryTrack, id, title, artist, album, path, file)
CAMEL_JSON_STRUCT(LibraryTrackPage, revision, total, tracks)
CAMEL_JSON_STRUCT(LibraryAlbumResult, id, title, artist)
CAMEL_JSON_STRUCT(LibrarySearchResults, revision, tracks, totalAlbums, totalArtists, albums, artists)
CAMEL_JSON_STRUCT(PlayPeriod, fromMs, toMs)
CAMEL_JSON_STRUCT(PlayStatisticsQuery, period, metric, bucket, topLimit, historyOffset, historyLimit)
CAMEL_JSON_STRUCT(PlayOverview, listenedMs, plays, tracks, albums, artists)
CAMEL_JSON_STRUCT(PlayRankedTrack, track, title, artist, album, plays, listenedMs, available, path, name)
CAMEL_JSON_STRUCT(PlayRankedGroup, album, name, artist, plays, listenedMs)
CAMEL_JSON_STRUCT(PlayBucket, start, plays, listenedMs)
CAMEL_JSON_STRUCT(PlayHistoryEntry, event, track, startedMs, utcOffsetMinutes, heardMs, counted, title, artist, album, available,
                  path, name, zone)
CAMEL_JSON_STRUCT(PlayStatistics, overview, tracks, albums, artists, buckets, historyTotal, history)
CAMEL_JSON_STRUCT(LibraryGroupFilter, field, value, role)
CAMEL_JSON_STRUCT(LibraryAlbum, id, folder, title, artist, year, tracks, discs, added, durationMs, favorite)
CAMEL_JSON_STRUCT(LibraryAlbumPage, revision, total, albums)
CAMEL_JSON_STRUCT(LibraryGroup, name, albums, tracks, favorite, field)
CAMEL_JSON_STRUCT(LibraryGroupPage, revision, total, groups)
CAMEL_JSON_STRUCT(LibraryTrackRow, id, track, title, artist, album, path, format, year, disc, number, durationMs, name, favorite)
CAMEL_JSON_STRUCT(LibraryTrackRowPage, revision, total, tracks)
CAMEL_JSON_STRUCT(LibraryAlbumFormat, format, bitsPerSample, sampleRate, tracks)
CAMEL_JSON_STRUCT(LibraryAlbumDetail, revision, album, tracks, formats)
CAMEL_JSON_STRUCT(LibraryArtistPage, revision, tracks, songs, albums, appearances, favorite)
CAMEL_JSON_STRUCT(LibraryFolderEntry, id, name, path, location, tracks)
CAMEL_JSON_STRUCT(LibraryFolderListing, revision, folder, trail, folders, tracks)
CAMEL_JSON_STRUCT(LibraryRootInfo, id, path, name, location, online, lastScan)
CAMEL_JSON_STRUCT(LibraryRootContents, tracks, plays, favorites, playlistEntries)
CAMEL_JSON_STRUCT(LibraryInitial, label, offset)
CAMEL_JSON_STRUCT(LibraryNamePart, text, name)
CAMEL_JSON_STRUCT(LibraryScanProgress, scans, found, checked, finishing)
CAMEL_JSON_STRUCT(LoudnessProgress, analyzed, total, running)
CAMEL_JSON_STRUCT(ArtworkRequest, kind, id, path, name, pixels)
CAMEL_JSON_STRUCT(ArtistCandidate, mbid, name, description, albums)
CAMEL_JSON_STRUCT(ArtistPicture, chosen, found, source, author, license, licenseUrl, page, mbid, candidates)
CAMEL_JSON_STRUCT(PlaylistSource, kind, id, track, value)
CAMEL_JSON_STRUCT(SelectionPreview, tracks, durationMs, unavailable, duplicates)
CAMEL_JSON_STRUCT(PlaylistUndo, playlist, revision, created, items)
CAMEL_JSON_STRUCT(PlaylistChange, playlist, revision, name, added, skippedUnavailable, skippedDuplicates, skippedExisting, undo)
CAMEL_JSON_STRUCT(PlaylistSummary, id, revision, tracks, created, modified, name)
CAMEL_JSON_STRUCT(PlaylistItem, id, position, added, durationMs, track, source, title, artist, album, path, name, available,
                  favorite)
CAMEL_JSON_STRUCT(PlaylistPage, playlist, revision, total, items)
CAMEL_JSON_STRUCT(PlaylistEdit, kind, playlist, revision, name, items, position, key, descending)
CAMEL_JSON_STRUCT(LyricLine, ms, text)
CAMEL_JSON_STRUCT(Lyrics, synced, lines)
CAMEL_JSON_STRUCT(FileTag, key, field, values, library)
CAMEL_JSON_STRUCT(FileTagSection, format, tags)
CAMEL_JSON_STRUCT(FileDetails, path, problem, reason, folderPresent, inLibrary, sections, library, format, sampleRate,
                  bitsPerSample, channels, bitrateKbps, frames, lengthMs, size, modified, trackGain, trackPeak, albumGain,
                  albumPeak, analysisTrackGain, analysisTrackPeak, analysisAlbumGain, analysisAlbumPeak)
CAMEL_JSON_STRUCT(DataRecovery, settings, library)
CAMEL_JSON_STRUCT(DiscordStatus, connection, cover, coverError)
CAMEL_JSON_STRUCT(DiscordTexts, bitPerfect, lossless, processed, lossy)

} // namespace camel::server
