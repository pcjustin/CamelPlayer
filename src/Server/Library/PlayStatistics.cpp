// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/PlayStatistics.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <sqlite3.h>

namespace camel::server {
namespace {

constexpr uint64_t kCountSeconds = 240;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int32_t utcOffsetMinutes() {
    FILETIME utc{}, local{};
    GetSystemTimeAsFileTime(&utc);
    if (!FileTimeToLocalFileTime(&utc, &local)) return 0;
    const auto ticks = [](FILETIME t) { return int64_t((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return int32_t((ticks(local) - ticks(utc)) / (10'000'000LL * 60));
}

std::string periodFilter(const PlayPeriod& period, const char* column) {
    std::string where = " WHERE 1";
    if (period.fromMs) where += std::string(" AND ") + column + ">=" + std::to_string(*period.fromMs);
    if (period.toMs) where += std::string(" AND ") + column + "<" + std::to_string(*period.toMs);
    return where;
}

} // namespace

PlaySession::PlaySession(LibraryTrackKey track, uint32_t sampleRate, uint64_t length, uint64_t rangeFirst,
                         std::optional<uint64_t> rangeEnd)
    : track_(track), sampleRate_(sampleRate), length_(length), rangeFirst_(rangeFirst), rangeEnd_(rangeEnd) {
    if (!sampleRate_) throw std::invalid_argument("play session needs a sample rate");
}

void PlaySession::credit(uint64_t from, uint64_t to) {
    from = std::max(from, rangeFirst_);
    if (rangeEnd_) to = std::min(to, *rangeEnd_);
    if (to <= from) return;
    if (!startedMs_) {
        startedMs_ = nowMs();
        utcOffsetMinutes_ = utcOffsetMinutes();
    }
    heard_ += to - from;
    const uint64_t threshold = std::min(length_ ? length_ / 2 : UINT64_MAX, kCountSeconds * sampleRate_);
    counted_ = counted_ || heard_ >= threshold;
}

void PlaySession::observe(uint64_t position, uint64_t maxStep) {
    if (last_ && position >= *last_ && position - *last_ <= maxStep) credit(*last_, position);
    last_ = position;
}

void PlaySession::interrupt() {
    last_.reset();
}

void PlaySession::finish() {
    const uint64_t end = rangeEnd_ ? *rangeEnd_ : length_;
    if (last_ && end > *last_) credit(*last_, end);
    last_.reset();
    if (!length_ && heard_ && !counted_) counted_ = true;
}

void PlaySession::discard() {
    heard_ = persistedMs_ = 0;
    counted_ = persistedCount_ = false;
    startedMs_ = 0;
    event_ = std::make_shared<int64_t>(0);
}

uint64_t PlaySession::heardMs() const {
    return heard_ * 1000 / sampleRate_;
}

void PlaySession::persist(LibraryStore& store) {
    const uint64_t heardMs = this->heardMs();
    if (!heardMs || (heardMs == persistedMs_ && counted_ == persistedCount_)) return;
    const auto addMs = int64_t(heardMs - persistedMs_);
    const bool addCount = counted_ && !persistedCount_;
    persistedMs_ = heardMs;
    persistedCount_ = counted_;
    store.write([event = event_, track = track_, started = startedMs_, offset = utcOffsetMinutes_, heardMs, counted = counted_,
                 addMs, addCount](sqlite::Database& db) {
        if (!*event) {
            db.prepare("INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES(?,?,?,?,?)")
                .bind(1, track.file)
                .bind(2, started)
                .bind(3, int64_t(offset))
                .bind(4, int64_t(heardMs))
                .bind(5, int64_t(counted))
                .run();
            *event = db.lastInsertId();
        } else {
            db.prepare("UPDATE play_events SET heard_ms=?,counted=? WHERE id=?")
                .bind(1, int64_t(heardMs))
                .bind(2, int64_t(counted))
                .bind(3, *event)
                .run();
        }
        db.prepare("INSERT INTO stats(file_id,play_count,last_played,listened_ms) VALUES(?,?,?,?) "
                   "ON CONFLICT(file_id) DO UPDATE SET play_count=play_count+excluded.play_count,"
                   "listened_ms=listened_ms+excluded.listened_ms,"
                   "last_played=coalesce(max(last_played,excluded.last_played),last_played,excluded.last_played)")
            .bind(1, track.file)
            .bind(2, int64_t(addCount))
            .bind(3, started)
            .bind(4, addMs)
            .run();
    });
}

PlayStatistics playStatistics(const LibraryStore& store, const PlayStatisticsQuery& query, const CancellationToken& token) {
    if (query.topLimit < 1 || query.topLimit > 500 || query.historyOffset < 0 || query.historyLimit < 0 ||
        query.historyLimit > 500 || (query.period.fromMs && query.period.toMs && *query.period.toMs < *query.period.fromMs))
        throw std::invalid_argument("invalid play statistics query");
    if (token.cancelled()) throw std::runtime_error("play statistics query cancelled");
    auto db = store.openReader();
    sqlite3_progress_handler(
        db.handle(), 1000, [](void* context) { return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0; },
        const_cast<CancellationToken*>(&token));
    db.exec("BEGIN");
    const std::string where = periodFilter(query.period, "e.started");
    const std::string order = query.metric == PlayMetric::Plays ? " ORDER BY plays DESC,ms DESC" : " ORDER BY ms DESC,plays DESC";
    const std::string available = "(f.available=1 AND r.online=1 AND r.removed IS NULL)";
    // Per-track totals in the period; artist rows fall back to the track's ARTIST when no multivalue rows exist.
    // CROSS JOIN keeps the played tracks first: left to the planner, it walked every artist value of the library (250 ms).
    const std::string totals = "WITH e AS (SELECT e.file_id,SUM(e.counted) plays,SUM(e.heard_ms) ms "
                               "FROM play_events e" +
                               where + " GROUP BY e.file_id) ";
    const std::string artists = totals +
                                ",a AS (SELECT e.plays,e.ms,v.value name FROM e CROSS JOIN tracks t ON t.file_id=e.file_id "
                                "CROSS JOIN track_values v ON v.track_id=t.id AND v.field='artist' "
                                "UNION ALL SELECT e.plays,e.ms,t.artist FROM e JOIN tracks t ON t.file_id=e.file_id "
                                "WHERE coalesce(t.artist,'')<>'' AND NOT EXISTS("
                                "SELECT 1 FROM track_values v WHERE v.track_id=t.id AND v.field='artist')) ";

    PlayStatistics out;
    auto overview = db.prepare("SELECT coalesce(SUM(e.heard_ms),0),coalesce(SUM(e.counted),0),"
                               "COUNT(DISTINCT e.file_id),COUNT(DISTINCT t.album_id) "
                               "FROM play_events e LEFT JOIN tracks t ON t.file_id=e.file_id" +
                               where);
    overview.step();
    out.overview = {overview.integer(0), overview.integer(1), overview.integer(2), overview.integer(3), 0};
    out.overview.artists = db.scalar(artists + "SELECT COUNT(DISTINCT name) FROM a").value_or(0);

    auto tracks = db.prepare(totals +
                             "SELECT e.file_id,coalesce(t.title,''),coalesce(t.artist,''),"
                             "coalesce(t.album,''),e.plays,e.ms," +
                             available +
                             ",f.path,coalesce(f.name,'') FROM e "
                             "LEFT JOIN tracks t ON t.file_id=e.file_id "
                             "JOIN files f ON f.id=e.file_id JOIN roots r ON r.id=f.root_id" +
                             order + ",e.file_id LIMIT ?");
    tracks.bind(1, query.topLimit);
    while (tracks.step())
        out.tracks.push_back({{tracks.integer(0)},
                              tracks.text(1),
                              tracks.text(2),
                              tracks.text(3),
                              tracks.integer(4),
                              tracks.integer(5),
                              tracks.integer(6) != 0,
                              tracks.text(7),
                              tracks.text(8)});

    auto albums = db.prepare(totals +
                             "SELECT al.id,coalesce(al.title,''),coalesce(al.album_artist,''),SUM(e.plays) plays,"
                             "SUM(e.ms) ms FROM e JOIN tracks t ON t.file_id=e.file_id "
                             "JOIN albums al ON al.id=t.album_id GROUP BY al.id" +
                             order + ",al.id LIMIT ?");
    albums.bind(1, query.topLimit);
    while (albums.step())
        out.albums.push_back({albums.integer(0), albums.text(1), albums.text(2), albums.integer(3), albums.integer(4)});

    auto artistRows =
        db.prepare(artists + "SELECT name,SUM(plays) plays,SUM(ms) ms FROM a GROUP BY name" + order + ",name LIMIT ?");
    artistRows.bind(1, query.topLimit);
    while (artistRows.step()) out.artists.push_back({0, artistRows.text(0), {}, artistRows.integer(1), artistRows.integer(2)});

    const char* localDay = "date((e.started+e.utc_offset*60000)/1000,'unixepoch')";
    const std::string bucket = query.bucket == PlayBucketSize::Day ? std::string(localDay)
                               : query.bucket == PlayBucketSize::Week
                                   ? std::string("date(") + localDay + ",'-6 days','weekday 1')"
                                   : std::string("strftime('%Y-%m-01',") + localDay + ")";
    auto buckets = db.prepare("SELECT " + bucket + " b,SUM(e.counted),SUM(e.heard_ms) FROM play_events e" + where +
                              " GROUP BY b ORDER BY b");
    while (buckets.step()) out.buckets.push_back({buckets.text(0), buckets.integer(1), buckets.integer(2)});

    out.historyTotal = db.scalar("SELECT COUNT(*) FROM play_events e" + where).value_or(0);
    auto history = db.prepare("SELECT e.id,e.file_id,e.started,e.utc_offset,e.heard_ms,e.counted,"
                              "coalesce(t.title,''),coalesce(t.artist,''),coalesce(t.album,'')," +
                              available +
                              ",f.path,coalesce(f.name,'') FROM play_events e LEFT JOIN tracks t ON t.file_id=e.file_id "
                              "JOIN files f ON f.id=e.file_id JOIN roots r ON r.id=f.root_id" +
                              where + " ORDER BY e.started DESC,e.id DESC LIMIT ? OFFSET ?");
    history.bind(1, query.historyLimit).bind(2, query.historyOffset);
    while (history.step())
        out.history.push_back({history.integer(0),
                               {history.integer(1)},
                               history.integer(2),
                               int32_t(history.integer(3)),
                               history.integer(4),
                               history.integer(5) != 0,
                               history.text(6),
                               history.text(7),
                               history.text(8),
                               history.integer(9) != 0,
                               history.text(10),
                               history.text(11)});
    db.exec("COMMIT");
    return out;
}

void resetPlayStatistics(LibraryStore& store, const std::vector<LibraryTrackKey>& tracks) {
    store
        .write([&](sqlite::Database& db) {
            if (tracks.empty()) {
                db.exec("DELETE FROM play_events; UPDATE stats SET play_count=0,listened_ms=0,last_played=NULL;");
                return;
            }
            auto events = db.prepare("DELETE FROM play_events WHERE file_id=?");
            auto stats = db.prepare("UPDATE stats SET play_count=0,listened_ms=0,last_played=NULL WHERE file_id=?");
            for (const auto& track : tracks) {
                events.bind(1, track.file).run();
                events.reset();
                stats.bind(1, track.file).run();
                stats.reset();
            }
        })
        .get();
}

void setTrackFavorite(LibraryStore& store, const std::vector<LibraryTrackKey>& tracks, bool favorite) {
    store
        .write([&](sqlite::Database& db) {
            // A track that already is a favorite keeps the time it became one.
            auto mark =
                db.prepare("INSERT INTO stats(file_id,favorite,favorite_at) VALUES(?1,?2,?3) ON CONFLICT(file_id) DO UPDATE "
                           "SET favorite_at=CASE WHEN favorite<>0 AND ?2 THEN favorite_at ELSE ?3 END,favorite=?2");
            for (const auto& track : tracks) {
                mark.bind(1, track.file).bind(2, int64_t(favorite));
                if (favorite)
                    mark.bind(3, nowMs());
                else
                    mark.bindNull(3);
                mark.run();
                mark.reset();
            }
            db.exec("UPDATE library_revision SET value=value+1 WHERE id=1");
        })
        .get();
}

void setAlbumFavorite(LibraryStore& store, const std::vector<int64_t>& albums, bool favorite) {
    store
        .write([&](sqlite::Database& db) {
            auto mark =
                db.prepare(favorite ? "INSERT INTO stats(file_id,album_favorite,album_favorite_at) SELECT file_id,1,?2 FROM "
                                      "tracks WHERE album_id=?1 ON CONFLICT(file_id) DO UPDATE SET album_favorite_at="
                                      "CASE WHEN album_favorite<>0 THEN album_favorite_at ELSE ?2 END,album_favorite=1"
                                    : "UPDATE stats SET album_favorite=0,album_favorite_at=NULL WHERE file_id IN "
                                      "(SELECT file_id FROM tracks WHERE album_id=?1)");
            const int64_t at = nowMs();
            for (const auto album : albums) {
                mark.bind(1, album);
                if (favorite) mark.bind(2, at);
                mark.run();
                mark.reset();
            }
            db.exec("UPDATE library_revision SET value=value+1 WHERE id=1");
        })
        .get();
}

} // namespace camel::server
