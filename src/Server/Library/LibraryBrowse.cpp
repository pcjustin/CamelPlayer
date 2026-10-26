// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/LibraryBrowse.h"

#include <algorithm>
#include <stdexcept>
#include <sqlite3.h>
#include "Server/Library/LibraryText.h"

namespace camel::server {
namespace {

// Search plans (docs/Plan.md §12.1). One full-text pass collects up to kHits match ids; the tracks, albums and artists
// of a narrower query are then read from those ids. A broader query walks the sort order and tests each row's text,
// stopping once the page is full: "nig" in a 100k library matches 40k titles, 330 ms ordered against under 1 ms walked.
// With at least 2% of the rows matching, a walk to row kWalkRows reads about 10k rows; deeper pages order every match.
constexpr int64_t kHits = 2000, kWalkRows = 200;

// library_tracks spelled out over `t`, so the walk can name the tracks index the view hides.
constexpr const char* kVisibleTrack =
    " f.available=1 AND r.online=1 AND (t.cue_index>0 OR (t.cue_index=0 AND NOT EXISTS"
    "(SELECT 1 FROM tracks v WHERE v.file_id=t.file_id AND v.cue_index<>0))) ";
// What tracks_fts indexes, matched as the trigram phrase matches it: a substring of the normalized text in ?1.
constexpr const char* kTextMatch =
    " (instr(coalesce(t.title_norm,t.title,''),?1)>0 OR instr(coalesce(t.album_norm,t.album,''),?1)>0 "
    "OR instr(coalesce(t.artist_norm,t.artist,''),?1)>0) ";
constexpr const char* kTracks = " FROM library_tracks t JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id ";
constexpr const char* kFtsTracks = " FROM tracks_fts JOIN library_tracks t ON t.id=tracks_fts.rowid "
                                   "JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id ";
constexpr const char* kVisible = " f.available=1 AND r.online=1 ";

struct Search {
    enum class Plan { Browse, Like, Hits, Walk, Fts } plan = Plan::Browse;
    std::string text;    // normalized query, matched by instr in Walk
    std::string phrase;  // FTS5 phrase for Fts
    std::string pattern; // LIKE pattern for one or two characters
    std::string hits;    // JSON array of matching track ids for Hits

    // The filter and its first bound parameter: ?1 for every plan.
    std::string filter() const {
        switch (plan) {
        case Plan::Browse: return {};
        case Plan::Like:
            return " AND (t.title_norm LIKE ?1 ESCAPE '\\' OR t.artist_norm LIKE ?1 ESCAPE '\\' OR t.album_norm LIKE ?1 "
                   "ESCAPE '\\') ";
        case Plan::Hits: return " AND t.id IN (SELECT value FROM json_each(?1)) ";
        case Plan::Walk: return std::string(" AND") + kTextMatch;
        case Plan::Fts: return " AND tracks_fts MATCH ?1 ";
        }
        return {};
    }
    const char* from() const { return plan == Plan::Fts ? kFtsTracks : kTracks; }
    void bind(sqlite::Statement& statement) const {
        switch (plan) {
        case Plan::Browse: break;
        case Plan::Like: statement.bind(1, pattern); break;
        case Plan::Hits: statement.bind(1, hits); break;
        case Plan::Walk: statement.bind(1, text); break;
        case Plan::Fts: statement.bind(1, phrase); break;
        }
    }
    // Exact totals of a walked query need every match, which only the full-text index lists cheaply.
    Search counting() const {
        Search all = *this;
        if (plan == Plan::Walk) all.plan = Plan::Fts;
        return all;
    }
};

Search plan(sqlite::Database& db, std::string_view filter, int64_t offset, int64_t limit) {
    Search search;
    if (filter.empty()) return search;
    search.text = filter;
    const auto characters =
        std::count_if(filter.begin(), filter.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    if (characters < 3) {
        search.plan = Search::Plan::Like;
        search.pattern = "%";
        for (char c : filter) {
            if (c == '%' || c == '_' || c == '\\') search.pattern.push_back('\\');
            search.pattern.push_back(c);
        }
        search.pattern.push_back('%');
        return search;
    }
    search.phrase = "\"";
    for (char c : filter) {
        search.phrase.push_back(c);
        if (c == '\"') search.phrase.push_back(c);
    }
    search.phrase.push_back('\"');
    auto rows = db.prepare("SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ? LIMIT ?");
    rows.bind(1, search.phrase).bind(2, kHits + 1);
    int64_t count = 0;
    search.hits = "[";
    while (rows.step()) {
        if (count++) search.hits.push_back(',');
        search.hits += std::to_string(rows.integer(0));
    }
    search.hits.push_back(']');
    search.plan = count <= kHits             ? Search::Plan::Hits
                  : offset + limit <= kWalkRows ? Search::Plan::Walk
                                                : Search::Plan::Fts;
    return search;
}

void cancellable(sqlite::Database& db, const CancellationToken& token) {
    sqlite3_progress_handler(db.handle(), 1000, [](void* context) {
        return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0;
    }, const_cast<CancellationToken*>(&token));
}

LibraryTrackPage tracks(sqlite::Database& db, const Search& search, int64_t offset, int64_t limit, bool totals,
                        const CancellationToken& token) {
    LibraryTrackPage result;
    const bool walk = search.plan == Search::Plan::Walk;
    // A search counts its matches while ordering them; browsing and walking count separately.
    const bool windowTotal = totals && !walk && search.plan != Search::Plan::Browse;
    const std::string columns = std::string("SELECT t.id,coalesce(t.title,''),coalesce(t.artist,''),coalesce(t.album,''),"
                                            "f.path,t.file_id") + (windowTotal ? ",count(*) OVER()" : "");
    const std::string select =
        walk ? columns + " FROM tracks t INDEXED BY tracks_title_sort JOIN files f ON f.id=t.file_id "
                         "JOIN roots r ON r.id=f.root_id WHERE" + kTextMatch + "AND" + kVisibleTrack
             : columns + search.from() + " WHERE" + kVisible + search.filter();
    auto rows = db.prepare(select + " ORDER BY t.title_sort,t.id LIMIT ?2 OFFSET ?3");
    search.bind(rows);
    rows.bind(2, limit).bind(3, offset);
    bool hasRow = rows.step();
    if (hasRow && windowTotal) result.total = rows.integer(6);
    if (totals && !(hasRow && windowTotal)) {
        const auto all = search.counting();
        auto count = db.prepare(std::string("SELECT count(*)") + all.from() + " WHERE" + kVisible + all.filter());
        all.bind(count);
        if (count.step()) result.total = count.integer(0);
    }
    for (; hasRow; hasRow = rows.step()) {
        if (token.cancelled()) throw std::runtime_error("library query cancelled");
        result.tracks.push_back({rows.integer(0), rows.text(1), rows.text(2), rows.text(3), rows.text(4), rows.integer(5)});
    }
    return result;
}

LibraryTrackPage page(const LibraryStore& store, std::string_view filter, int64_t offset, int64_t limit,
                      std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    if (offset < 0 || limit < 1 || limit > 500) throw std::invalid_argument("invalid library page range");
    if (token.cancelled()) throw std::runtime_error("library query cancelled");
    auto db = store.openReader();
    cancellable(db, token);
    db.exec("BEGIN");
    const auto revision = uint64_t(db.scalar("SELECT value FROM library_revision WHERE id=1").value_or(0));
    if (expectedRevision && *expectedRevision != revision) throw std::runtime_error("library query revision changed");
    auto result = tracks(db, plan(db, filter, offset, limit), offset, limit, true, token);
    result.revision = revision;
    if (token.cancelled()) throw std::runtime_error("library query cancelled");
    db.exec("COMMIT");
    return result;
}

} // namespace

LibraryTrackPage browseTracks(const LibraryStore& store, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision,
                              const CancellationToken& token) {
    return page(store, {}, offset, limit, expectedRevision, token);
}

LibraryTrackPage searchTracks(const LibraryStore& store, std::string query, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    return page(store, normalizeLibrarySearch(query), offset, limit, expectedRevision, token);
}

LibrarySearchResults searchLibrary(const LibraryStore& store, std::string query, int64_t offset, int64_t limit,
                                   std::optional<uint64_t> expectedRevision, const CancellationToken& token, bool totals) {
    if (offset < 0 || limit < 1 || limit > 500) throw std::invalid_argument("invalid library page range");
    if (token.cancelled()) throw std::runtime_error("library query cancelled");
    auto result = LibrarySearchResults{};
    const auto normalized = normalizeLibrarySearch(query);
    auto db = store.openReader();
    cancellable(db, token);
    db.exec("BEGIN");
    result.revision = uint64_t(db.scalar("SELECT value FROM library_revision WHERE id=1").value_or(0));
    if (expectedRevision && *expectedRevision != result.revision)
        throw std::runtime_error("library query revision changed");
    const auto search = plan(db, normalized, offset, limit);
    result.tracks = tracks(db, search, offset, limit, totals, token);
    result.tracks.revision = result.revision;
    if (normalized.empty()) return result;

    // Albums with a matching visible track; a walk tests each album's tracks in album order.
    {
        const bool like = search.plan == Search::Plan::Like;
        const std::string albumFilter =
            like ? " AND (t.album_norm LIKE ?1 ESCAPE '\\' OR t.artist_norm LIKE ?1 ESCAPE '\\') " : search.filter();
        const std::string albumFrom = std::string(search.from()) + "JOIN albums a ON a.id=t.album_id ";
        auto rows = db.prepare(
            search.plan == Search::Plan::Walk
                ? std::string("SELECT a.id,coalesce(a.title,''),coalesce(a.album_artist,'') FROM albums a INDEXED BY "
                              "albums_sort WHERE EXISTS (SELECT 1 FROM tracks t JOIN files f ON f.id=t.file_id "
                              "JOIN roots r ON r.id=f.root_id WHERE t.album_id=a.id AND") +
                      kTextMatch + "AND" + kVisibleTrack + ") ORDER BY a.sort_key,a.id LIMIT ?2 OFFSET ?3"
                : "SELECT id,title,artist FROM (SELECT a.id,coalesce(a.title,'') title,coalesce(a.album_artist,'') artist,"
                  "a.sort_key" + albumFrom + "WHERE" + kVisible + albumFilter + " GROUP BY a.id) "
                  "ORDER BY sort_key,id LIMIT ?2 OFFSET ?3");
        search.bind(rows);
        rows.bind(2, limit).bind(3, offset);
        while (rows.step()) result.albums.push_back({rows.integer(0), rows.text(1), rows.text(2)});
        if (totals) {
            const auto all = search.counting();
            const std::string allFilter = like ? albumFilter : all.filter();
            auto total = db.prepare(std::string("SELECT COUNT(DISTINCT a.id)") + all.from() +
                                    "JOIN albums a ON a.id=t.album_id WHERE" + kVisible + allFilter);
            all.bind(total);
            if (total.step()) result.totalAlbums = total.integer(0);
        }
    }

    // Artists whose name matches; each multi-artist track lists every artist it carries.
    {
        const std::string artist = "coalesce(nullif(v.value,''),t.artist,'')";
        Search named = search;
        std::string artistFilter;
        switch (search.plan) {
        case Search::Plan::Like:
            artistFilter = " AND (t.artist_norm LIKE ?1 ESCAPE '\\' OR v.value_norm LIKE ?1 ESCAPE '\\') ";
            break;
        case Search::Plan::Hits:
            artistFilter = search.filter() + " AND instr(coalesce(t.artist_norm,t.artist,''),?4)>0 ";
            break;
        default:
            named.plan = Search::Plan::Fts;
            named.phrase = "artist_norm : " + search.phrase;
            artistFilter = named.filter();
        }
        const std::string artistFrom = std::string(named.from()) +
                                       "LEFT JOIN track_values v ON v.track_id=t.id AND v.field='artist' ";
        auto rows = db.prepare("SELECT name FROM (SELECT " + artist + " name,min(coalesce(v.sort_key,t.artist_sort)) sort_key" +
                               artistFrom + "WHERE" + kVisible + artistFilter + " GROUP BY " + artist +
                               ") ORDER BY sort_key,name LIMIT ?2 OFFSET ?3");
        named.bind(rows);
        rows.bind(2, limit).bind(3, offset);
        if (search.plan == Search::Plan::Hits) rows.bind(4, search.text);
        while (rows.step()) result.artists.push_back(rows.text(0));
        if (totals) {
            auto total = db.prepare("SELECT COUNT(DISTINCT " + artist + ")" + artistFrom + "WHERE" + kVisible + artistFilter);
            named.bind(total);
            if (search.plan == Search::Plan::Hits) total.bind(4, search.text);
            if (total.step()) result.totalArtists = total.integer(0);
        }
    }
    if (token.cancelled()) throw std::runtime_error("library query cancelled");
    db.exec("COMMIT");
    return result;
}

} // namespace camel::server
