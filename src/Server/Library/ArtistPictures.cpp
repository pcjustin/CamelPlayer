// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/ArtistPictures.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>

#include "Common/util/Text.h"
#include "Server/Artwork/ArtworkCache.h"

namespace camel::server {

namespace {

using json = nlohmann::json;

constexpr int64_t kMinute = 60'000;
constexpr int64_t kRetryAfter = int64_t{30} * 24 * 60 * kMinute;

int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Row {
    ArtistPicture picture;
    std::optional<std::string> outcome; // none before the first lookup
    bool online = false;
    int64_t checked = 0;
};

Row readRow(const LibraryStore& store, const std::string& name) {
    auto db = store.openReader();
    auto query = db.prepare("SELECT chosen IS NOT NULL,found IS NOT NULL,source,outcome,online,checked,mbid,candidates,"
                            "author,license,license_url,page FROM artist_pictures WHERE name=?");
    Row row;
    if (!query.bind(1, name).step()) return row;
    auto& p = row.picture;
    p.chosen = query.integer(0) != 0;
    p.found = query.integer(1) != 0;
    p.source = query.text(2);
    if (!query.isNull(3)) row.outcome = query.text(3);
    row.online = query.integer(4) != 0;
    row.checked = query.integer(5);
    p.mbid = query.text(6);
    if (!query.isNull(7))
        for (const auto& c : json::parse(query.text(7)))
            p.candidates.push_back({c.value("mbid", ""), c.value("name", ""), c.value("description", "")});
    p.author = query.text(8);
    p.license = query.text(9);
    p.licenseUrl = query.text(10);
    p.page = query.text(11);
    return row;
}

bool due(const Row& row, bool online, int64_t at) {
    if (row.picture.chosen || row.picture.found) return false;
    if (!row.outcome) return true;
    const auto age = at - row.checked;
    if (*row.outcome == "failed") return age >= kMinute;
    if (*row.outcome == "ambiguous") return online && age >= kRetryAfter;
    return (online && !row.online) || age >= kRetryAfter;
}

// What a lookup keeps; the found picture and its attribution only with `replaceFound`.
struct Record {
    std::string outcome;
    bool online = false;
    int64_t checked = 0;
    std::string mbid;
    std::vector<ArtistCandidate> candidates;
    bool replaceFound = false;
    std::vector<uint8_t> found; // empty clears it
    std::string source, author, license, licenseUrl, page;
};

void record(LibraryStore& store, const std::string& name, const Record& r) {
    store
        .write([&](sqlite::Database& db) {
            db.prepare("INSERT INTO artist_pictures(name) VALUES(?) ON CONFLICT(name) DO NOTHING").bind(1, name).run();
            auto update = db.prepare("UPDATE artist_pictures SET outcome=?2,online=?3,checked=?4,mbid=?5,candidates=?6 "
                                     "WHERE name=?1");
            update.bind(1, name).bind(2, r.outcome).bind(3, int64_t(r.online)).bind(4, r.checked);
            if (r.mbid.empty())
                update.bindNull(5);
            else
                update.bind(5, r.mbid);
            if (r.candidates.empty()) {
                update.bindNull(6);
            } else {
                auto list = json::array();
                for (const auto& c : r.candidates)
                    list.push_back({{"mbid", c.mbid}, {"name", c.name}, {"description", c.description}});
                update.bind(6, list.dump());
            }
            update.run();
            if (!r.replaceFound) return;
            auto picture = db.prepare("UPDATE artist_pictures SET found=?2,source=?3,author=?4,license=?5,license_url=?6,"
                                      "page=?7,changed=max(changed+1,?8) WHERE name=?1");
            picture.bind(1, name);
            if (r.found.empty())
                picture.bindNull(2).bindNull(3);
            else
                picture.bind(2, std::span<const uint8_t>(r.found)).bind(3, r.source);
            picture.bind(4, r.author).bind(5, r.license).bind(6, r.licenseUrl).bind(7, r.page).bind(8, now());
            picture.run();
        })
        .get();
}

Record recordOf(const Row& row) {
    Record r;
    r.mbid = row.picture.mbid;
    r.candidates = row.picture.candidates;
    return r;
}

// Takes what the online lookup found into `r`. Candidates come only from a search; a known artist keeps the ones of
// the search that offered it.
void take(Record& r, online::Found& found) {
    r.mbid = found.mbid;
    if (!found.candidates.empty())
        r.candidates = found.candidates.size() > 1 ? std::move(found.candidates) : decltype(r.candidates){};
    if (r.mbid.empty() && r.candidates.size() > 1) {
        r.outcome = "ambiguous";
        return;
    }
    r.outcome = "none";
    if (!found.image || found.bytes.empty()) return;
    try {
        r.found = squarePicture(found.bytes, kArtistPicturePixels);
    } catch (const std::exception&) {
        return;
    }
    r.outcome = "found";
    r.replaceFound = true;
    r.source = "commons";
    r.author = found.image->author;
    r.license = found.image->license;
    r.licenseUrl = found.image->licenseUrl;
    r.page = found.image->page;
}

std::vector<uint8_t> readFile(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || !size || size > kMaxArtworkBytes) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return {};
    return bytes;
}

struct Local {
    std::vector<uint8_t> bytes;
    std::string source;
};

std::optional<Local> localPicture(const LibraryStore& store, const std::string& name, const CancellationToken& token) {
    std::vector<std::filesystem::path> images, tracks;
    {
        auto db = store.openReader();
        auto files = db.prepare("SELECT f.path FROM folders d JOIN files f ON f.folder_id=d.id JOIN roots r ON r.id=f.root_id "
                                "WHERE d.name=? AND f.kind='image' AND f.available=1 AND r.online=1 AND r.removed IS NULL");
        files.bind(1, name);
        while (files.step()) images.emplace_back(toWide(files.text(0)));
        // A few of the tracks credited to the name alone, from different albums.
        auto credited = db.prepare("SELECT f.path FROM track_values v JOIN tracks t ON t.id=v.track_id JOIN files f ON "
                                   "f.id=t.file_id JOIN roots r ON r.id=f.root_id WHERE v.field='artist' AND v.value=? AND "
                                   "f.available=1 AND r.online=1 AND r.removed IS NULL GROUP BY t.album_id LIMIT 3");
        credited.bind(1, name);
        while (credited.step()) tracks.emplace_back(toWide(credited.text(0)));
    }
    for (const auto* wanted : {L"artist.jpg", L"artist.jpeg", L"artist.png", L"artist.webp"})
        for (const auto& image : images) {
            auto file = image.filename().wstring();
            std::transform(file.begin(), file.end(), file.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (file != wanted || token.cancelled()) continue;
            if (auto bytes = readFile(image); !bytes.empty()) return Local{std::move(bytes), "folder"};
        }
    const auto embedded = makeEmbeddedArtistPictureProvider();
    for (const auto& track : tracks) {
        if (token.cancelled()) return std::nullopt;
        ArtworkQuery query;
        query.firstTrack = track;
        try {
            auto found = embedded->find(query, token);
            if (!found.empty()) return Local{std::move(found.front().bytes), embedded->id()};
        } catch (const std::exception&) {
            // An unreadable track is passed over.
        }
    }
    return std::nullopt;
}

} // namespace

ArtistPicture artistPicture(const LibraryStore& store, const std::string& name) {
    return readRow(store, name).picture;
}

ArtistPicture lookUpArtistPicture(LibraryStore& store, const std::string& name, bool online, const online::Services& services,
                                  const CancellationToken& token) {
    const auto row = readRow(store, name);
    const auto at = now();
    if (name.empty() || !due(row, online, at)) return row.picture;
    auto r = recordOf(row);
    r.online = online;
    r.checked = at;
    // The library was searched at the last lookup; a NAS takes seconds to read tracks again just to go online.
    const bool searched = row.outcome && at - row.checked < kRetryAfter;
    if (auto local = searched ? std::nullopt : localPicture(store, name, token)) {
        try {
            r.found = squarePicture(local->bytes, kArtistPicturePixels);
            r.replaceFound = true;
            r.source = local->source;
            r.outcome = "found";
        } catch (const std::exception&) {
            // An unreadable local picture leaves the online lookup to find one.
        }
    }
    if (token.cancelled()) return row.picture;
    if (!r.replaceFound) {
        if (!online) {
            r.outcome = "none";
        } else {
            try {
                auto found = r.mbid.empty() ? online::findArtistPicture(name, services, token)
                                            : online::pictureOf(r.mbid, services, token);
                take(r, found);
            } catch (const std::exception&) {
                if (token.cancelled()) return row.picture;
                r.outcome = "failed";
            }
        }
    }
    record(store, name, r);
    return artistPicture(store, name);
}

ArtistPicture setArtistPicture(LibraryStore& store, const std::string& name, const std::vector<uint8_t>& image) {
    if (name.empty()) throw std::invalid_argument("no artist name");
    const auto picture = squarePicture(image, kArtistPicturePixels);
    store
        .write([&](sqlite::Database& db) {
            db.prepare("INSERT INTO artist_pictures(name,chosen,changed) VALUES(?1,?2,?3) ON CONFLICT(name) DO UPDATE SET "
                       "chosen=excluded.chosen,changed=max(changed+1,excluded.changed)")
                .bind(1, name)
                .bind(2, std::span<const uint8_t>(picture))
                .bind(3, now())
                .run();
        })
        .get();
    return artistPicture(store, name);
}

ArtistPicture removeArtistPicture(LibraryStore& store, const std::string& name) {
    store
        .write([&](sqlite::Database& db) {
            db.prepare("UPDATE artist_pictures SET chosen=NULL,changed=max(changed+1,?2) WHERE name=?1 AND chosen IS NOT NULL")
                .bind(1, name)
                .bind(2, now())
                .run();
        })
        .get();
    return artistPicture(store, name);
}

ArtistPicture chooseArtistCandidate(LibraryStore& store, const std::string& name, const std::string& mbid,
                                    const online::Services& services, const CancellationToken& token) {
    const auto row = readRow(store, name);
    if (std::ranges::none_of(row.picture.candidates, [&](const ArtistCandidate& c) { return c.mbid == mbid; }))
        throw std::invalid_argument("not one of the name's artists");
    auto r = recordOf(row);
    r.online = true;
    r.checked = now();
    auto found = online::pictureOf(mbid, services, token);
    take(r, found);
    r.replaceFound = true; // the picture of another artist of the name goes
    record(store, name, r);
    return artistPicture(store, name);
}

} // namespace camel::server
