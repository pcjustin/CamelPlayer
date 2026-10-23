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

LibraryTrackPage page(const LibraryStore& store, std::string_view filter, int64_t offset, int64_t limit,
                      std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    if (offset < 0 || limit < 1 || limit > 500) throw std::invalid_argument("invalid library page range");
    if (token.cancelled()) throw std::runtime_error("library query cancelled");

    auto db = store.openReader();
    sqlite3_progress_handler(db.handle(), 1000, [](void* context) {
        return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0;
    }, const_cast<CancellationToken*>(&token));
    db.exec("BEGIN");
    LibraryTrackPage result;
    result.revision = uint64_t(db.scalar("SELECT value FROM library_revision WHERE id=1").value_or(0));
    if (expectedRevision && *expectedRevision != result.revision) throw std::runtime_error("library query revision changed");

    const bool searching = !filter.empty();
    const bool trigram = std::count_if(filter.begin(), filter.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) >= 3;
    std::string from = trigram
                           ? " FROM tracks_fts JOIN library_tracks t ON t.id=tracks_fts.rowid "
                             "JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id "
                           : " FROM library_tracks t JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id ";
    std::string where = " WHERE f.available=1 AND r.online=1 ";
    if (searching) {
        if (trigram) {
            where += " AND tracks_fts MATCH ? ";
        } else {
            where += " AND (t.title_norm LIKE ? ESCAPE '\\' OR t.artist_norm LIKE ? ESCAPE '\\' OR t.album_norm LIKE ? ESCAPE '\\') ";
        }
    }
    auto bindFilter = [&](sqlite::Statement& statement) {
        if (!searching) return;
        if (trigram) {
            std::string phrase = "\"";
            for (char c : filter) { phrase.push_back(c); if (c == '\"') phrase.push_back(c); }
            phrase.push_back('\"');
            statement.bind(1, phrase);
        } else {
            std::string pattern = "%";
            for (char c : filter) {
                if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\');
                pattern.push_back(c);
            }
            pattern.push_back('%');
            statement.bind(1, pattern).bind(2, pattern).bind(3, pattern);
        }
    };
    if (!searching) {
        auto count = db.prepare("SELECT count(*)" + from + where);
        bindFilter(count);
        if (count.step()) result.total = count.integer(0);
    }

    const std::string select = std::string("SELECT t.id,coalesce(t.title,''),coalesce(t.artist,''),coalesce(t.album,''),f.path") +
                               (searching ? ",count(*) OVER()" : "") + from + where +
                               " ORDER BY t.title_sort,t.id LIMIT ? OFFSET ?";
    auto rows = db.prepare(select);
    bindFilter(rows);
    const int base = searching ? (trigram ? 2 : 4) : 1;
    rows.bind(base, limit).bind(base + 1, offset);
    bool hasRow = rows.step();
    if (hasRow && searching) result.total = rows.integer(5);
    if (!hasRow && searching) {
        auto count = db.prepare("SELECT count(*)" + from + where);
        bindFilter(count);
        if (count.step()) result.total = count.integer(0);
    }
    if (hasRow) do {
        if (token.cancelled()) throw std::runtime_error("library query cancelled");
        result.tracks.push_back({rows.integer(0), rows.text(1), rows.text(2), rows.text(3), rows.text(4)});
    } while (rows.step());
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

} // namespace camel::server
