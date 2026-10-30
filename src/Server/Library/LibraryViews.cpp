// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/LibraryViews.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <sqlite3.h>

#include "Common/util/Text.h"
#include "Server/Library/LibraryText.h"

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

    Query(const LibraryStore& store, std::optional<uint64_t> expected, const CancellationToken& token) : db(store.openReader()) {
        if (token.cancelled()) throw std::runtime_error("library query cancelled");
        sqlite3_progress_handler(
            db.handle(), 1000, [](void* context) { return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0; },
            const_cast<CancellationToken*>(&token));
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
    return {rows.integer(0),  {rows.integer(1)}, rows.text(2),         rows.text(3),    rows.text(4),
            rows.text(5),     rows.text(6),      rows.integer(7),      rows.integer(8), rows.integer(9),
            rows.integer(12), rows.text(10),     rows.integer(11) != 0};
}

LibraryAlbum album(sqlite::Statement& rows) {
    return {rows.integer(0), rows.integer(1), rows.text(2),    rows.text(3),    rows.integer(4),
            rows.integer(5), rows.integer(6), rows.integer(7), rows.integer(8), rows.integer(9) != 0};
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
    if (entries.size() > 16) entries.pop_back(); // an artist page keeps three: its tracks and two album rows
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
    "coalesce(MAX(t.disc_number),1),coalesce(MIN(f.added),a.added),SUM(";
constexpr const char* kAlbumFavorite = "),MAX(coalesce(s.album_favorite,0))";

// The library's names for one revision, read once, so album artists divide as track credits do.
std::shared_ptr<const LibraryNameSplitter> namesAt(const LibraryStore& store, Query& q) {
    static std::mutex mutex;
    static std::string key;
    static std::shared_ptr<const LibraryNameSplitter> names;
    const auto wanted = toUtf8(store.path().wstring()) + "|" + std::to_string(q.revision);
    std::lock_guard lock(mutex);
    if (!names || key != wanted) {
        names = std::make_shared<const LibraryNameSplitter>(loadLibraryNames(q.db));
        key = wanted;
    }
    return names;
}

// Albums whose album artist credits `name`, visible or not.
std::string creditedAlbums(const LibraryStore& store, Query& q, const std::string& name) {
    std::string ids;
    std::shared_ptr<const LibraryNameSplitter> names;
    auto rows = q.db.prepare("SELECT id,album_artist FROM albums WHERE instr(album_artist,?)>0");
    rows.bind(1, name);
    while (rows.step()) {
        if (rows.text(1) != name) {
            if (!names) names = namesAt(store, q);
            const auto credited = names->names(rows.text(1));
            if (std::find(credited.begin(), credited.end(), name) == credited.end()) continue;
        }
        ids += (ids.empty() ? "" : ",") + std::to_string(rows.integer(0));
    }
    return ids;
}

// The visible tracks a filter matches, as groupCondition does, read once per library revision. The lookup index finds
// the tracks holding the value among their values; only the single-column match walks every track.
std::shared_ptr<const std::vector<int64_t>> groupTracks(const LibraryStore& store, Query& q, const LibraryGroupFilter& filter) {
    const auto condition = groupCondition(filter);
    return ordered<int64_t>(store, q.revision, "groupTracks|" + condition, [&] {
        const std::string from = std::string("SELECT t.id FROM tracks t") + kJoins + "WHERE" + kVisible;
        if (filter.field == LibraryGroupField::Year) return ids(q.db, from + condition);
        const std::string column = fieldName(filter.field), field = literal(column), value = literal(filter.value);
        return ids(q.db, from + " AND t.id IN (SELECT track_id FROM track_values WHERE field=" + field +
                             " AND value_norm=" + literal(normalizeLibrarySearch(filter.value)) + " AND value=" + value +
                             " UNION ALL SELECT id FROM tracks c WHERE c." + column + "=" + value +
                             " AND NOT EXISTS(SELECT 1 FROM track_values v WHERE v.track_id=c.id AND v.field=" + field + "))");
    });
}

std::string albumCondition(const LibraryStore& store, Query& q, const LibraryGroupFilter& filter) {
    if (filter.role == LibraryAlbumRole::Any || filter.field == LibraryGroupField::Genre ||
        filter.field == LibraryGroupField::Year)
        return groupCondition(filter);
    const auto own = creditedAlbums(store, q, filter.value);
    if (filter.role == LibraryAlbumRole::Own) return " AND a.id IN (" + own + ")";
    const auto tracks = groupTracks(store, q, filter);
    return " AND t.id IN (" + idList(*tracks, 0, int64_t(tracks->size())) + ") AND a.id NOT IN (" + own + ")";
}

} // namespace

std::string libraryDisplayPath(const std::string& key) {
    std::wstring text = toWide(key);
    wil::unique_handle handle(CreateFileW(text.c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle) {
        std::wstring resolved(32768, L'\0');
        const DWORD size = GetFinalPathNameByHandleW(handle.get(), resolved.data(), DWORD(resolved.size()),
                                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size && size < resolved.size()) text.assign(resolved.data(), size);
    }
    if (_wcsnicmp(text.c_str(), LR"(\\?\UNC\)", 8) == 0)
        text = LR"(\\)" + text.substr(8);
    else if (text.starts_with(LR"(\\?\)"))
        text = text.substr(4);
    return toUtf8(text);
}

std::string libraryLocation(const std::string& key) {
    auto display = libraryDisplayPath(key);
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

namespace {

constexpr const char* kAlbumOrders[] = {"a.sort_key", "lower(coalesce(a.album_artist,'')),a.sort_key",
                                        "coalesce(a.year,0) DESC,a.sort_key",
                                        "coalesce(MIN(f.added),a.added) DESC,a.sort_key"}; // an album joined with its first file

std::string albumsFrom(const std::string& condition) {
    return std::string(" FROM albums a JOIN tracks t ON t.album_id=a.id") + kJoins + kStats + "WHERE" + kVisible + condition;
}

// A group's name, track, album and sort key for each visible track.
std::string groupValues(LibraryGroupField field) {
    if (field == LibraryGroupField::Year)
        return std::string("SELECT CAST(t.year AS TEXT) name,t.id track,t.album_id album,-t.year sort FROM tracks t") + kJoins +
               "WHERE" + kVisible + " AND t.year>0";
    const std::string column = fieldName(field);
    const std::string sortFallback = field == LibraryGroupField::Artist ? "t.artist_sort" : "NULL";
    return "SELECT coalesce(nullif(v.value,''),t." + column + ") name,t.id track,t.album_id album,coalesce(v.sort_key," +
           sortFallback + ") sort FROM tracks t" + kJoins +
           "LEFT JOIN track_values v ON v.track_id=t.id AND v.field=" + literal(column) + " WHERE" + kVisible;
}

// Labels each position of an ordered list with the last initial whose start does not follow it; `starts` holds where
// "A" … "Z", "あ" and "漢" begin in the list's own ordering, everything before "A" being "#".
template <typename Key> std::vector<LibraryInitial> initialsOf(const std::vector<Key>& keys, const std::vector<Key>& starts) {
    const auto& labels = libraryInitialLabels();
    std::vector<LibraryInitial> out;
    size_t initial = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        while (initial < starts.size() && !(keys[i] < starts[initial])) ++initial;
        if (out.empty() || out.back().label != labels[initial]) out.push_back({labels[initial], int64_t(i)});
    }
    return out;
}

// Where the initials begin among the library's sort keys (LCMapString in its sort locale): the smaller key of each
// letter's two cases, the smallest kana and the smallest of the Han characters that open the stroke and the
// pronunciation orders.
std::vector<std::vector<uint8_t>> sortKeyStarts(sqlite::Database& db) {
    auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
    const std::wstring locale = state.step() ? toWide(state.text(0)) : L"zh-TW";
    const auto smallest = [&](std::initializer_list<const char*> texts) {
        std::vector<uint8_t> least;
        for (const auto* text : texts) {
            auto key = librarySortKey(text, locale);
            if (least.empty() || key < least) least = std::move(key);
        }
        return least;
    };
    std::vector<std::vector<uint8_t>> starts;
    for (char c = 'A'; c <= 'Z'; ++c)
        starts.push_back(smallest({std::string(1, c).c_str(), std::string(1, char(c + 32)).c_str()}));
    starts.push_back(smallest({"\xE3\x81\x81", "\xE3\x81\x82", "\xE3\x82\xA1", "\xE3\x82\xA2", "\xEF\xBD\xB1"})); // ぁあァアｱ
    starts.push_back(smallest({"\xE4\xB8\x80", "\xE4\xB9\x99", "\xE5\x85\xAB", "\xE5\xB7\xB4", "\xE9\x98\xBF"})); // 一乙八巴阿
    return starts;
}

} // namespace

LibraryAlbumPage browseAlbums(const LibraryStore& store, AlbumSort sort, const std::optional<LibraryGroupFilter>& filter,
                              int64_t offset, int64_t limit, std::optional<uint64_t> expectedRevision,
                              const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    const auto& orders = kAlbumOrders;
    const std::string condition = filter ? albumCondition(store, q, *filter) : std::string{};
    const std::string from = albumsFrom(condition);
    const auto order = ordered<int64_t>(store, q.revision, "albums|" + std::to_string(int(sort)) + "|" + condition, [&] {
        return ids(q.db, "SELECT a.id" + from + " GROUP BY a.id ORDER BY " + orders[size_t(sort)] + ",a.id");
    });
    LibraryAlbumPage page;
    page.revision = q.revision;
    page.total = int64_t(order->size());
    const auto slice = idList(*order, offset, limit);
    if (slice.empty()) return page;
    std::vector<LibraryAlbum> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite + from + " AND a.id IN (" +
                                  slice + ") GROUP BY a.id");
    while (statement.step()) rows.push_back(album(statement));
    page.albums = inOrder(*order, offset, limit, std::move(rows), [](const LibraryAlbum& a) { return a.id; });
    return page;
}

LibraryAlbumPage recentlyPlayedAlbums(const LibraryStore& store, int64_t offset, int64_t limit, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, std::nullopt, token);
    // CROSS JOIN starts from the play events; the planner otherwise walks every track of the library.
    const std::string played = std::string(" FROM play_events e CROSS JOIN tracks t ON t.file_id=e.file_id") + kJoins + "WHERE" +
                               kVisible + " AND t.album_id IS NOT NULL";
    LibraryAlbumPage page;
    page.revision = q.revision;
    page.total = q.db.scalar("SELECT COUNT(DISTINCT t.album_id)" + played).value_or(0);
    std::vector<int64_t> order;
    auto recent = q.db.prepare("SELECT t.album_id" + played +
                               " GROUP BY t.album_id ORDER BY MAX(e.started) DESC,t.album_id LIMIT ? OFFSET ?");
    recent.bind(1, limit).bind(2, offset);
    while (recent.step()) order.push_back(recent.integer(0));
    if (order.empty()) return page;
    const auto count = int64_t(order.size());
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
    const std::string values = groupValues(field);
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

const std::vector<std::string>& libraryInitialLabels() {
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> out{"#"};
        for (char c = 'A'; c <= 'Z'; ++c) out.emplace_back(1, c);
        out.emplace_back("\xE3\x81\x82"); // あ
        out.emplace_back("\xE6\xBC\xA2"); // 漢
        return out;
    }();
    return labels;
}

std::shared_ptr<const LibraryCreditNames>
libraryCreditNames(const LibraryStore& store, std::shared_ptr<const LibraryCreditNames> cached, const CancellationToken& token) {
    Query q(store, std::nullopt, token);
    if (cached && cached->revision == q.revision) return cached;
    auto names = std::make_shared<LibraryCreditNames>();
    names->revision = q.revision;
    names->splitter = *namesAt(store, q);
    auto artists = q.db.prepare("SELECT DISTINCT value FROM track_values WHERE field='artist'");
    while (artists.step()) names->artists.insert(artists.text(0));
    return names;
}

std::vector<LibraryNamePart> creditParts(const LibraryCreditNames& names, std::string_view credit) {
    auto parts = names.splitter.parts(credit);
    for (auto& part : parts) part.name = part.name && names.artists.contains(part.text);
    return parts;
}

std::vector<LibraryInitial> groupInitials(const LibraryStore& store, LibraryGroupField field, const CancellationToken& token) {
    if (field == LibraryGroupField::Year) return {};
    Query q(store, std::nullopt, token);
    std::vector<std::vector<uint8_t>> keys;
    auto rows = q.db.prepare("SELECT min(sort) FROM (" + groupValues(field) +
                             ") WHERE coalesce(name,'')<>'' GROUP BY name ORDER BY min(sort),name");
    while (rows.step()) keys.push_back(rows.blob(0));
    return initialsOf(keys, sortKeyStarts(q.db));
}

std::vector<LibraryInitial> albumInitials(const LibraryStore& store, AlbumSort sort, const CancellationToken& token) {
    if (sort != AlbumSort::Title && sort != AlbumSort::Artist) return {};
    Query q(store, std::nullopt, token);
    const std::string query = std::string("SELECT ") +
                              (sort == AlbumSort::Title ? "a.sort_key" : "lower(coalesce(a.album_artist,''))") + albumsFrom({}) +
                              " GROUP BY a.id ORDER BY " + kAlbumOrders[size_t(sort)] + ",a.id";
    auto rows = q.db.prepare(query);
    if (sort == AlbumSort::Title) {
        std::vector<std::vector<uint8_t>> keys;
        while (rows.step()) keys.push_back(rows.blob(0));
        return initialsOf(keys, sortKeyStarts(q.db));
    }
    // Album artists sort as lowercased UTF-8 bytes: kana begins at U+3040 and what follows Bopomofo (U+3100) is Han.
    std::vector<std::string> names;
    while (rows.step()) names.push_back(rows.text(0));
    std::vector<std::string> starts;
    for (char c = 'a'; c <= 'z'; ++c) starts.emplace_back(1, c);
    starts.emplace_back("\xE3\x81\x80");
    starts.emplace_back("\xE3\x84\x80");
    return initialsOf(names, starts);
}

LibraryTrackRowPage browseTrackRows(const LibraryStore& store, TrackSort sort, bool descending,
                                    const std::optional<LibraryGroupFilter>& filter, int64_t offset, int64_t limit,
                                    std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    const std::string condition = filter ? groupCondition(*filter) : std::string{};
    const std::string direction = descending ? " DESC" : "";
    const std::string orders[] = {"t.title_sort" + direction,
                                  "t.artist_sort" + direction + ",t.album_sort,t.disc_number,t.track_number",
                                  "t.album_sort" + direction + ",t.disc_number,t.track_number",
                                  "coalesce(t.year,0)" + direction + ",t.album_sort,t.disc_number,t.track_number",
                                  std::string(kDuration) + direction,
                                  "coalesce(f.added,0)" + direction + ",t.id" + direction};
    const auto order = ordered<int64_t>(store, q.revision, "tracks|" + std::to_string(int(sort)) + direction + condition, [&] {
        return ids(q.db, std::string("SELECT t.id FROM tracks t") + kJoins + "WHERE" + kVisible + condition + " ORDER BY " +
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

LibraryArtistPage artistPage(const LibraryStore& store, const LibraryGroupFilter& artist, int64_t songs, int64_t albums,
                             const CancellationToken& token) {
    checkRange(0, songs);
    checkRange(0, albums);
    LibraryArtistPage page;
    {
        Query q(store, std::nullopt, token);
        page.revision = q.revision;
        const auto tracks = groupTracks(store, q, artist);
        page.tracks = int64_t(tracks->size());
        if (!tracks->empty()) {
            auto rows = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                                     "WHERE t.id IN (" + idList(*tracks, 0, page.tracks) +
                                     ") ORDER BY coalesce(s.play_count,0) DESC,coalesce(s.listened_ms,0) DESC,"
                                     "coalesce(t.year,0),t.album_sort,t.disc_number,t.track_number,t.id LIMIT ?");
            rows.bind(1, songs);
            while (rows.step()) page.songs.push_back(trackRow(rows));
        }
    }
    auto own = artist, guest = artist;
    own.role = LibraryAlbumRole::Own;
    guest.role = LibraryAlbumRole::Guest;
    page.albums = browseAlbums(store, AlbumSort::Year, own, 0, albums, std::nullopt, token);
    page.appearances = browseAlbums(store, AlbumSort::Year, guest, 0, albums, std::nullopt, token);
    return page;
}

LibraryTrackRowPage smartTracks(const LibraryStore& store, SmartList list, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    const std::string from = std::string(" FROM tracks t") + kJoins + kStats + "WHERE" + kVisible;
    const auto days =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 86400;
    std::string order;
    switch (list) {
    case SmartList::RecentlyAdded:
        order = "SELECT t.id" + from + " AND f.added>=" + std::to_string((days - 30) * 86400) +
                " ORDER BY f.added DESC,t.album_sort,t.disc_number,t.track_number,t.id";
        break;
    case SmartList::MostPlayed:
        order = "SELECT t.id" + from + " AND s.play_count>0 ORDER BY s.play_count DESC,s.listened_ms DESC,t.id LIMIT 100";
        break;
    case SmartList::RecentlyPlayed:
        order = "SELECT t.id" + from +
                " AND EXISTS(SELECT 1 FROM play_events e WHERE e.file_id=t.file_id) ORDER BY "
                "(SELECT MAX(e.started) FROM play_events e WHERE e.file_id=t.file_id) DESC,t.id LIMIT 100";
        break;
    case SmartList::Favorites:
        order = "SELECT t.id" + from + " AND s.favorite<>0 ORDER BY s.favorite_at DESC NULLS LAST,t.title_sort,t.id";
        break;
    case SmartList::NeverPlayed:
        order = "SELECT t.id" + from +
                " AND coalesce(s.play_count,0)=0 ORDER BY f.added DESC NULLS LAST,t.album_sort,"
                "t.disc_number,t.track_number,t.id";
        break;
    }
    // Plays and the date do not change the library revision, so they are part of the cached order's key.
    auto plays = q.db.prepare("SELECT coalesce(MAX(id),0)||':'||COUNT(*)||':'||(SELECT coalesce(SUM(play_count),0) FROM stats) "
                              "FROM play_events");
    plays.step();
    const auto key = "smart|" + std::to_string(int(list)) + "|" + plays.text(0) + "|" + std::to_string(days);
    const auto listed = ordered<int64_t>(store, q.revision, key, [&] { return ids(q.db, order); });
    LibraryTrackRowPage page;
    page.revision = q.revision;
    page.total = int64_t(listed->size());
    const auto slice = idList(*listed, offset, limit);
    if (slice.empty()) return page;
    std::vector<LibraryTrackRow> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                                  "WHERE t.id IN (" + slice + ")");
    while (statement.step()) rows.push_back(trackRow(statement));
    page.tracks = inOrder(*listed, offset, limit, std::move(rows), [](const LibraryTrackRow& t) { return t.id; });
    return page;
}

LibraryAlbumPage favoriteAlbums(const LibraryStore& store, int64_t offset, int64_t limit,
                                std::optional<uint64_t> expectedRevision, const CancellationToken& token) {
    checkRange(offset, limit);
    Query q(store, expectedRevision, token);
    const std::string from =
        std::string(" FROM albums a JOIN tracks t ON t.album_id=a.id") + kJoins + kStats + "WHERE" + kVisible;
    const auto order = ordered<int64_t>(store, q.revision, "favoriteAlbums", [&] {
        return ids(q.db, "SELECT a.id" + from +
                             " GROUP BY a.id HAVING MAX(coalesce(s.album_favorite,0))=1 ORDER BY "
                             "MAX(s.album_favorite_at) DESC NULLS LAST,a.sort_key,a.id");
    });
    LibraryAlbumPage page;
    page.revision = q.revision;
    page.total = int64_t(order->size());
    const auto slice = idList(*order, offset, limit);
    if (slice.empty()) return page;
    std::vector<LibraryAlbum> rows;
    auto statement = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite + from + " AND a.id IN (" +
                                  slice + ") GROUP BY a.id");
    while (statement.step()) rows.push_back(album(statement));
    page.albums = inOrder(*order, offset, limit, std::move(rows), [](const LibraryAlbum& a) { return a.id; });
    return page;
}

LibraryAlbumDetail albumDetail(const LibraryStore& store, int64_t id, const CancellationToken& token) {
    Query q(store, std::nullopt, token);
    LibraryAlbumDetail detail;
    detail.revision = q.revision;
    auto header = q.db.prepare(std::string("SELECT ") + kAlbumColumns + kDuration + kAlbumFavorite +
                               " FROM albums a JOIN tracks t ON "
                               "t.album_id=a.id" +
                               kJoins + kStats + "WHERE a.id=? AND" + kVisible + " GROUP BY a.id");
    if (!header.bind(1, id).step()) throw std::invalid_argument("unknown or unavailable album");
    detail.album = album(header);
    auto rows = q.db.prepare(std::string("SELECT ") + kTrackColumns + kDuration + " FROM tracks t" + kJoins + kStats +
                             "WHERE t.album_id=? AND" + kVisible + " ORDER BY t.disc_number,t.track_number,f.path");
    rows.bind(1, id);
    while (rows.step()) detail.tracks.push_back(trackRow(rows));
    auto formats =
        q.db.prepare(std::string("SELECT coalesce(t.format,''),coalesce(t.bits_per_sample,0),coalesce(t.sample_rate,0),"
                                 "COUNT(*) FROM tracks t") +
                     kJoins + "WHERE t.album_id=? AND" + kVisible + " GROUP BY 1,2,3 ORDER BY 4 DESC,3,2,1");
    formats.bind(1, id);
    while (formats.step())
        detail.formats.push_back({formats.text(0), formats.integer(1), formats.integer(2), formats.integer(3)});
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
                                 "WHERE f.folder_id=? AND" + kVisible + " ORDER BY t.disc_number,t.track_number,f.path");
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
