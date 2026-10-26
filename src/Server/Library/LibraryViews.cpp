// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/LibraryViews.h"

#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <sqlite3.h>

#include "Common/util/Text.h"

namespace camel::server {
namespace {

constexpr const char* kJoins = " JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id ";
constexpr const char* kVisible = " f.available=1 AND r.online=1 AND r.removed IS NULL ";
constexpr const char* kDuration =
    "CASE WHEN t.sample_rate>0 THEN (coalesce(t.end_frame,t.total_frames,0)-t.start_frame)*1000/t.sample_rate ELSE 0 END";
constexpr const char* kTrackColumns =
    "t.id,t.file_id,coalesce(t.title,''),coalesce(t.artist,''),coalesce(t.album,''),f.path,coalesce(t.format,''),"
    "coalesce(t.year,0),coalesce(t.disc_number,0),coalesce(t.track_number,0),coalesce(f.name,''),coalesce(s.favorite,0),";
constexpr const char* kStats = " LEFT JOIN stats s ON s.file_id=t.file_id ";

// A reader transaction with cancellation; the revision check makes every page of one listing come from one library state.
struct Query {
    sqlite::Database db;
    uint64_t revision = 0;

    Query(const LibraryStore& store, std::optional<uint64_t> expected, const CancellationToken& token)
        : db(store.openReader()) {
        if (token.cancelled()) throw std::runtime_error("library query cancelled");
        sqlite3_progress_handler(db.handle(), 1000, [](void* context) {
            return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0;
        }, const_cast<CancellationToken*>(&token));
        db.exec("BEGIN");
        revision = uint64_t(db.scalar("SELECT value FROM library_revision WHERE id=1").value_or(0));
        if (expected && *expected != revision) throw std::runtime_error("library query revision changed");
    }
    ~Query() {
        try {
            db.exec("COMMIT");
        } catch (...) {
        }
    }
};

void checkRange(int64_t offset, int64_t limit) {
    if (offset < 0 || limit < 1 || limit > 500) throw std::invalid_argument("invalid library page range");
}

std::string literal(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        out.push_back(c);
        if (c == '\'') out.push_back(c);
    }
    return out + "'";
}

const char* fieldName(LibraryGroupField field) {
    return field == LibraryGroupField::Artist ? "artist" : field == LibraryGroupField::Composer ? "composer" : "genre";
}

// Tracks without multivalue rows match their single-valued column.
std::string groupCondition(const LibraryGroupFilter& filter) {
    if (filter.field == LibraryGroupField::Year) {
        size_t used = 0;
        const auto year = std::stoll(filter.value, &used);
        if (used != filter.value.size()) throw std::invalid_argument("invalid year filter");
        return " AND t.year=" + std::to_string(year);
    }
    const std::string field = literal(fieldName(filter.field)), value = literal(filter.value);
    return " AND (EXISTS(SELECT 1 FROM track_values v WHERE v.track_id=t.id AND v.field=" + field + " AND v.value=" + value +
           ") OR (NOT EXISTS(SELECT 1 FROM track_values v WHERE v.track_id=t.id AND v.field=" + field + ") AND t." +
           fieldName(filter.field) + "=" + value + "))";
}

LibraryTrackRow trackRow(sqlite::Statement& rows) {
    return {rows.integer(0), {rows.integer(1)}, rows.text(2), rows.text(3), rows.text(4), rows.text(5),
            rows.text(6), rows.integer(7), rows.integer(8), rows.integer(9), rows.integer(12), rows.text(10),
            rows.integer(11) != 0};
}

LibraryAlbum album(sqlite::Statement& rows) {
    return {rows.integer(0), rows.integer(1), rows.text(2), rows.text(3), rows.integer(4), rows.integer(5),
            rows.integer(6), rows.integer(7), rows.integer(8), rows.integer(9) != 0};
}

// Each listing's full order for one library revision, so a deep page is a slice instead of an OFFSET walk over the
// whole library; a scan or edit changes the revision and therefore builds a new order.
template <class T>
std::shared_ptr<const std::vector<T>> ordered(const LibraryStore& store, uint64_t revision, const std::string& listing,
                                              const std::function<std::vector<T>()>& build) {
    struct Entry {
        std::string key;
        std::shared_ptr<const std::vector<T>> order;
    };
    static std::mutex mutex;
    static std::list<Entry> entries;
    const auto key = toUtf8(store.path().wstring()) + "|" + std::to_string(revision) + "|" + listing;
    {
        std::lock_guard lock(mutex);
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->key == key) {
                entries.splice(entries.begin(), entries, it);
                return entries.front().order;
            }
    }
    auto order = std::make_shared<const std::vector<T>>(build());
    std::lock_guard lock(mutex);
    entries.push_front({key, order});
    if (entries.size() > 8) entries.pop_back();
    return order;
}

std::vector<int64_t> ids(sqlite::Database& db, const std::string& sql) {
    std::vector<int64_t> out;
    auto rows = db.prepare(sql);
    while (rows.step()) out.push_back(rows.integer(0));
    return out;
}

std::string idList(const std::vector<int64_t>& order, int64_t offset, int64_t limit) {
    std::string list;
    for (int64_t i = offset; i < std::min<int64_t>(offset + limit, int64_t(order.size())); ++i)
        list += (list.empty() ? "" : ",") + std::to_string(order[size_t(i)]);
    return list;
}

// Rows of a slice come back in key order; this restores the listing order.
template <class T, class Id>
std::vector<T> inOrder(const std::vector<int64_t>& order, int64_t offset, int64_t limit, std::vector<T> rows, Id id) {
    std::unordered_map<int64_t, size_t> index;
    for (size_t i = 0; i < rows.size(); ++i) index.emplace(id(rows[i]), i);
    std::vector<T> out;
    for (int64_t i = offset; i < std::min<int64_t>(offset + limit, int64_t(order.size())); ++i)
        if (const auto found = index.find(order[size_t(i)]); found != index.end()) out.push_back(std::move(rows[found->second]));
    return out;
}

constexpr const char* kAlbumColumns =
    "a.id,coalesce(a.folder_id,0),coalesce(a.title,''),coalesce(a.album_artist,''),coalesce(a.year,0),COUNT(*),"
    "coalesce(MAX(t.disc_number),1),a.added,SUM(";
constexpr const char* kAlbumFavorite = "),MAX(coalesce(s.album_favorite,0))";

} // namespace

std::string libraryDisplayPath(const std::string& key) {
    std::wstring text = toWide(key);
    wil::unique_handle handle(CreateFileW(text.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle) {
        std::wstring resolved(32768, L'\0');
        const DWORD size = GetFinalPathNameByHandleW(handle.get(), resolved.data(), DWORD(resolved.size()),
                                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size && size < resolved.size()) text.assign(resolved.data(), size);
    }
    if (_wcsnicmp(text.c_str(), LR"(\\?\UNC\)", 8) == 0) text = LR"(\\)" + text.substr(8);
    else if (text.starts_with(LR"(\\?\)")) text = text.substr(4);
    return toUtf8(text);
}

std::string libraryLocation(const std::string& key) {
    const auto display = libraryDisplayPath(key);
    const std::wstring text = toWide(display);
    if (text.size() < 2 || text[1] != L':') return display;
    const std::wstring drive{wchar_t(towupper(text[0])), L':'};
    std::wstring out;
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = ARRAYSIZE(computer);
    if (GetComputerNameW(computer, &size)) out = std::wstring(computer) + L" › ";
    out += L"[" + drive + L"]";
    wchar_t label[MAX_PATH + 1]{};
    if (GetVolumeInformationW((drive + L"\\").c_str(), label, ARRAYSIZE(label), nullptr, nullptr, nullptr, nullptr, 0) && *label)
        out += L" " + std::wstring(label);
    for (const auto& part : std::filesystem::path(text).relative_path()) out += L" › " + part.wstring();
    return toUtf8(out);
}

namespace {
// A stored key as text, for rows a scan has not named yet: no access to the music storage.
std::string keyText(const std::string& key) {
    if (_strnicmp(key.c_str(), R"(\\?\UNC\)", 8) == 0) return R"(\\)" + key.substr(8);
    return key.starts_with(R"(\\?\)") ? key.substr(4) : key;
}
std::string lastPart(const std::string& text) {
    return toUtf8(std::filesystem::path(toWide(text)).filename().wstring());
}
// A root's display path and location as its scan recorded them; before that, a local root asks its own disk and a
// share shows its key, so listing roots never waits for the network.
std::pair<std::string, std::string> rootText(const std::string& key, const std::string& display, const std::string& location) {
    if (!display.empty()) return {display, location.empty() ? display : location};
    const auto text = keyText(key);
    if (text.starts_with(R"(\)")) return {text, text};
    return {libraryDisplayPath(key), libraryLocation(key)};
}
} // namespace

LibraryAlbumPage browseAlbums(const LibraryStore& store, AlbumSort sort, const std::optional<LibraryGroupFilter>& filter,
                              int64_t offset, int64_t limit, std::optional<uint64_t> expectedRevision,
                              const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    static constexpr const char* orders[] = {"a.sort_key", "lower(coalesce(a.album_artist,'')),a.sort_key",
                                             "coalesce(a.year,0) DESC,a.sort_key", "a.added DESC,a.sort_key"};
    const std::string condition = filter ? groupCondition(*filter) : std::string{};
    const std::string from = std::string(" FROM albums a JOIN tracks t ON t.album_id=a.id") + kJoins + kStats + "WHERE" +
                             kVisible + condition;
    const auto order = ordered<int64_t>(store, q.revision, "albums|" + std::to_string(int(sort)) + "|" + condition, [&] {
        return ids(q.db, "SELECT a.id" + from + " GROUP BY a.id ORDER BY " + orders[size_t(sort)] + ",a.id");
    });
    LibraryAlbumPage page;
    page.revision = q.revision;
    page.total = int64_t(order->size());
    const auto slice = idList(*order, offset, limit);
    if (slice.empty()) return page;
    std::vector<LibraryAlbum> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite + from + " AND a.id IN (" + slice +
                                  ") GROUP BY a.id");
    while (statement.step()) rows.push_back(album(statement));
    page.albums = inOrder(*order, offset, limit, std::move(rows), [](const LibraryAlbum& a) { return a.id; });
    return page;
}

LibraryAlbumPage recentlyPlayedAlbums(const LibraryStore& store, int64_t offset, int64_t limit, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, std::nullopt, token);
    const std::string played =
        std::string(" FROM play_events e JOIN tracks t ON t.file_id=e.file_id") + kJoins +
        "WHERE" + kVisible + " AND t.album_id IS NOT NULL";
    LibraryAlbumPage page;
    page.revision = q.revision;
    page.total = q.db.scalar("SELECT COUNT(DISTINCT t.album_id)" + played).value_or(0);
    std::vector<int64_t> order;
    auto recent = q.db.prepare("SELECT t.album_id" + played +
                               " GROUP BY t.album_id ORDER BY MAX(e.started) DESC,t.album_id LIMIT ? OFFSET ?");
    recent.bind(1, limit).bind(2, offset);
    while (recent.step()) order.push_back(recent.integer(0));
    if (order.empty()) return page;
    const int64_t count = int64_t(order.size());
    std::vector<LibraryAlbum> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite +
                                  " FROM albums a JOIN tracks t ON t.album_id=a.id" + kJoins + kStats + "WHERE" + kVisible +
                                  " AND a.id IN (" + idList(order, 0, count) + ") GROUP BY a.id");
    while (statement.step()) rows.push_back(album(statement));
    page.albums = inOrder(order, 0, count, std::move(rows), [](const LibraryAlbum& a) { return a.id; });
    return page;
}

LibraryGroupPage browseGroups(const LibraryStore& store, LibraryGroupField field, int64_t offset, int64_t limit,
                              std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    std::string values;
    if (field == LibraryGroupField::Year) {
        values = std::string("SELECT CAST(t.year AS TEXT) name,t.id track,t.album_id album,-t.year sort FROM tracks t") +
                 kJoins + "WHERE" + kVisible + " AND t.year>0";
    } else {
        const std::string column = fieldName(field);
        const std::string sortFallback = field == LibraryGroupField::Artist ? "t.artist_sort" : "NULL";
        values = "SELECT coalesce(nullif(v.value,''),t." + column + ") name,t.id track,t.album_id album,"
                 "coalesce(v.sort_key," + sortFallback + ") sort FROM tracks t" + kJoins +
                 "LEFT JOIN track_values v ON v.track_id=t.id AND v.field=" + literal(column) + " WHERE" + kVisible;
    }
    const auto groups = ordered<LibraryGroup>(store, q.revision, "groups|" + std::to_string(int(field)), [&] {
        std::vector<LibraryGroup> out;
        auto rows = q.db.prepare("SELECT name,COUNT(DISTINCT album),COUNT(DISTINCT track) FROM (" + values +
                                 ") WHERE coalesce(name,'')<>'' GROUP BY name ORDER BY min(sort),name");
        while (rows.step()) out.push_back({rows.text(0), rows.integer(1), rows.integer(2)});
        return out;
    });
    LibraryGroupPage page;
    page.revision = q.revision;
    page.total = int64_t(groups->size());
    for (int64_t i = offset; i < std::min<int64_t>(offset + limit, page.total); ++i) page.groups.push_back((*groups)[size_t(i)]);
    return page;
}

LibraryTrackRowPage browseTrackRows(const LibraryStore& store, TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                    std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    const std::string direction = descending ? " DESC" : "";
    const std::string orders[] = {"t.title_sort" + direction,
                                  "t.artist_sort" + direction + ",t.album_sort,t.disc_number,t.track_number",
                                  "t.album_sort" + direction + ",t.disc_number,t.track_number",
                                  "coalesce(t.year,0)" + direction + ",t.album_sort,t.disc_number,t.track_number",
                                  std::string(kDuration) + direction,
                                  "t.id" + direction};
    const auto order = ordered<int64_t>(store, q.revision, "tracks|" + std::to_string(int(sort)) + direction, [&] {
        return ids(q.db, std::string("SELECT t.id FROM tracks t") + kJoins + "WHERE" + kVisible + " ORDER BY " +
                             orders[size_t(sort)] + ",t.id");
    });
    LibraryTrackRowPage page;
    page.revision = q.revision;
    page.total = int64_t(order->size());
    const auto slice = idList(*order, offset, limit);
    if (slice.empty()) return page;
    std::vector<LibraryTrackRow> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                                  "WHERE t.id IN (" + slice + ")");
    while (statement.step()) rows.push_back(trackRow(statement));
    page.tracks = inOrder(*order, offset, limit, std::move(rows), [](const LibraryTrackRow& t) { return t.id; });
    return page;
}

LibraryAlbumDetail albumDetail(const LibraryStore& store, int64_t id, const CancellationToken& token) {
    Query q(store, std::nullopt, token);
    LibraryAlbumDetail detail;
    detail.revision = q.revision;
    auto header = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite + " FROM albums a JOIN tracks t ON "
                               "t.album_id=a.id" + kJoins + kStats + "WHERE a.id=? AND" + kVisible + " GROUP BY a.id");
    if (!header.bind(1, id).step()) throw std::invalid_argument("unknown or unavailable album");
    detail.album = album(header);
    auto rows = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                             "WHERE t.album_id=? AND" + kVisible +
                             " ORDER BY t.disc_number,t.track_number,f.path");
    rows.bind(1, id);
    while (rows.step()) detail.tracks.push_back(trackRow(rows));
    return detail;
}

LibraryFolderListing browseFolder(const LibraryStore& store, std::optional<int64_t> folder, const CancellationToken& token) {
    Query q(store, std::nullopt, token);
    LibraryFolderListing listing;
    listing.revision = q.revision;
    listing.folder = folder;
    // Columns: id, path, name, root display, root location.
    const auto entry = [](sqlite::Statement& row, bool isRoot) {
        const auto path = row.text(1);
        if (isRoot) {
            const auto [shown, location] = rootText(path, row.text(3), row.text(4));
            return LibraryFolderEntry{row.integer(0), lastPart(shown), path, location};
        }
        return LibraryFolderEntry{row.integer(0), row.text(2).empty() ? lastPart(path) : row.text(2), path};
    };
    // A path range over the unique files.path index finds the first track below a folder without a LIKE pattern.
    auto folders = folder ? q.db.prepare("SELECT d.id,d.path,coalesce(d.name,''),coalesce(r.display,''),"
                                         "coalesce(r.location,'') FROM folders d JOIN roots r ON r.id=d.root_id "
                                         "WHERE d.parent_id=? AND r.removed IS NULL AND r.online=1 AND EXISTS("
                                         "SELECT 1 FROM files f JOIN tracks t ON t.file_id=f.id WHERE f.available=1 "
                                         "AND f.path>d.path||'\\' AND f.path<d.path||']') ORDER BY d.path")
                          : q.db.prepare("SELECT d.id,d.path,coalesce(d.name,''),coalesce(r.display,''),"
                                         "coalesce(r.location,'') FROM folders d JOIN roots r ON r.id=d.root_id "
                                         "WHERE d.parent_id IS NULL AND r.removed IS NULL AND r.online=1 ORDER BY d.path");
    if (folder) folders.bind(1, *folder);
    while (folders.step()) {
        if (token.cancelled()) throw std::runtime_error("library query cancelled");
        listing.folders.push_back(entry(folders, !folder));
    }
    if (folder) {
        auto up = q.db.prepare("SELECT d.id,d.path,coalesce(d.name,''),coalesce(r.display,''),coalesce(r.location,''),"
                               "d.parent_id FROM folders d JOIN roots r ON r.id=d.root_id WHERE d.id=?");
        for (std::optional<int64_t> id = *folder; id;) {
            up.reset();
            if (!up.bind(1, *id).step()) throw std::invalid_argument("unknown library folder");
            const std::optional<int64_t> parent = up.isNull(5) ? std::nullopt : std::optional(up.integer(5));
            listing.trail.insert(listing.trail.begin(), entry(up, !parent));
            id = parent;
        }
        auto rows = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                                 "WHERE f.folder_id=? AND" + kVisible +
                                 " ORDER BY t.disc_number,t.track_number,f.path");
        rows.bind(1, *folder);
        while (rows.step()) listing.tracks.push_back(trackRow(rows));
    }
    return listing;
}

std::vector<LibraryRootInfo> libraryRoots(const LibraryStore& store) {
    auto db = store.openReader();
    auto rows = db.prepare("SELECT id,path,online,coalesce(last_scan,0),coalesce(display,''),coalesce(location,'') FROM roots "
                           "WHERE removed IS NULL ORDER BY path");
    std::vector<LibraryRootInfo> out;
    while (rows.step()) {
        const auto [shown, location] = rootText(rows.text(1), rows.text(4), rows.text(5));
        out.push_back({rows.integer(0), rows.text(1), lastPart(shown), location, rows.integer(2) != 0, rows.integer(3)});
    }
    return out;
}

} // namespace camel::server
