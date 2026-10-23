// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/LibraryText.h"

#include <windows.h>
#define Opencc_BUILT_AS_STATIC
#include <opencc/SimpleConverter.hpp>
#include <filesystem>
#include <stdexcept>
#include "Common/util/Text.h"
#include "Server/Library/Sqlite.h"
#include "Server/Library/LibraryStore.h"

#pragma comment(lib, "normaliz.lib")

namespace camel::server {
namespace {

std::wstring map(std::wstring_view text, DWORD flags) {
    if (text.empty()) return {};
    int count = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count) throw std::runtime_error("cannot normalize library text");
    std::wstring out(size_t(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), int(text.size()), out.data(), count, nullptr, nullptr, 0))
        throw std::runtime_error("cannot normalize library text");
    return out;
}

std::string converterConfig() {
    std::wstring executable(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (!count || count >= executable.size()) throw std::runtime_error("cannot locate OpenCC dictionaries");
    executable.resize(count);
    return toUtf8((std::filesystem::path(executable).parent_path() / L"opencc" / L"t2s.json").wstring());
}

} // namespace

std::string normalizeLibrarySearch(std::string_view text) {
    if (text.empty()) return {};
    auto wide = toWide(std::string(text));
    int count = NormalizeString(NormalizationKC, wide.data(), int(wide.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("cannot normalize library Unicode");
    std::wstring normalized(size_t(count), L'\0');
    count = NormalizeString(NormalizationKC, wide.data(), int(wide.size()), normalized.data(), count);
    if (count <= 0) throw std::runtime_error("cannot normalize library Unicode");
    normalized.resize(size_t(count));
    normalized = map(map(normalized, LCMAP_HIRAGANA), LCMAP_LOWERCASE);
    thread_local opencc::SimpleConverter converter(converterConfig());
    return converter.Convert(toUtf8(normalized));
}

std::vector<uint8_t> librarySortKey(std::string_view text, std::wstring_view locale) {
    const std::wstring name(locale);
    if (name != L"zh-TW" && name != L"zh-TW_pronun") throw std::invalid_argument("unsupported library sort locale");
    const auto wide = toWide(std::string(text));
    const int count = LCMapStringEx(name.c_str(), LCMAP_SORTKEY, wide.c_str(), -1, nullptr, 0, nullptr, nullptr, 0);
    if (!count) throw std::invalid_argument("unsupported library sort locale");
    std::vector<uint8_t> key(size_t(count), 0);
    if (!LCMapStringEx(name.c_str(), LCMAP_SORTKEY, wide.c_str(), -1, reinterpret_cast<wchar_t*>(key.data()), count,
                       nullptr, nullptr, 0)) throw std::runtime_error("cannot generate library sort key");
    return key;
}

std::vector<uint8_t> libraryNlsVersion(std::wstring_view locale) {
    NLSVERSIONINFOEX version{};
    version.dwNLSVersionInfoSize = sizeof(version);
    const std::wstring name(locale);
    if (name != L"zh-TW" && name != L"zh-TW_pronun") throw std::invalid_argument("unsupported library sort locale");
    if (!GetNLSVersionEx(COMPARE_STRING, name.c_str(), &version)) throw std::invalid_argument("unsupported library sort locale");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&version);
    return {bytes, bytes + sizeof(version)};
}

void updateLibraryTrackText(sqlite::Database& db, int64_t track, std::wstring_view locale) {
    std::wstring selected(locale);
    if (selected.empty()) {
        auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
        selected = state.step() ? toWide(state.text(0)) : L"zh-TW";
    }
    auto row = db.prepare("SELECT coalesce(title,''),coalesce(album,''),coalesce(artist,''),"
                          "coalesce(nullif(title_sort_tag,''),title,''),coalesce(nullif(album_sort_tag,''),album,''),"
                          "coalesce(nullif(artist_sort_tag,''),artist,'') FROM tracks WHERE id=?");
    row.bind(1, track);
    if (!row.step()) return;
    auto update = db.prepare("UPDATE tracks SET title_norm=?,album_norm=?,artist_norm=?,title_sort=?,album_sort=?,artist_sort=? WHERE id=?");
    for (int i = 0; i < 3; ++i) {
        update.bind(i + 1, normalizeLibrarySearch(row.text(i)));
        const auto key = librarySortKey(row.text(i + 3), selected);
        update.bind(i + 4, key);
    }
    update.bind(7, track).run();
    auto values = db.prepare("SELECT field,position,value,coalesce(nullif(sort_source,''),value) FROM track_values WHERE track_id=?");
    values.bind(1, track);
    while (values.step()) {
        const auto key = librarySortKey(values.text(3), selected);
        db.prepare("UPDATE track_values SET value_norm=?,sort_key=? WHERE track_id=? AND field=? AND position=?")
            .bind(1, normalizeLibrarySearch(values.text(2))).bind(2, key).bind(3, track)
            .bind(4, values.text(0)).bind(5, values.integer(1)).run();
    }
}

void updateLibraryAlbumText(sqlite::Database& db, int64_t id, std::wstring_view locale) {
    std::wstring selected(locale);
    if (selected.empty()) {
        auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
        selected = state.step() ? toWide(state.text(0)) : L"zh-TW";
    }
    auto album = db.prepare("SELECT title,coalesce(album_artist,''),coalesce((SELECT nullif(album_sort_tag,'') FROM tracks WHERE album_id=? ORDER BY id LIMIT 1),title) FROM albums WHERE id=?");
    album.bind(1, id).bind(2, id);
    if (album.step()) {
        const auto key = librarySortKey(album.text(2), selected);
        db.prepare("UPDATE albums SET title_norm=?,artist_norm=?,sort_key=? WHERE id=?")
            .bind(1, normalizeLibrarySearch(album.text(0))).bind(2, normalizeLibrarySearch(album.text(1)))
            .bind(3, key).bind(4, id).run();
    }
}

LibraryTextIndex::LibraryTextIndex(LibraryStore& store,
                                 std::function<std::vector<uint8_t>(std::wstring_view)> version)
    : store_(store), version_(std::move(version)) {
    auto db = store_.openReader();
    auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
    requested_ = state.step() ? toWide(state.text(0)) : L"zh-TW";
    libraryNlsVersion(requested_);
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

LibraryTextIndex::~LibraryTextIndex() {
    worker_.request_stop();
    cv_.notify_all();
    worker_.join();
}

void LibraryTextIndex::setLocale(std::wstring locale) {
    libraryNlsVersion(locale);
    {
        std::lock_guard lock(mutex_);
        requested_ = std::move(locale);
        ++generation_;
        pending_ = true;
    }
    cv_.notify_all();
}

void LibraryTextIndex::waitIdle() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !pending_ && !running_; });
    if (failure_) std::rethrow_exception(failure_);
}

void LibraryTextIndex::run(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        std::wstring locale;
        uint64_t generation;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return pending_ || stop.stop_requested(); });
            if (stop.stop_requested()) return;
            locale = requested_;
            generation = generation_;
            pending_ = false;
            running_ = true;
            failure_ = nullptr;
        }
        auto obsolete = [&] {
            std::lock_guard lock(mutex_);
            return stop.stop_requested() || generation_ != generation;
        };
        try {
            const auto version = version_(locale);
            std::string signature = toUtf8(locale) + ":";
            constexpr char hex[] = "0123456789abcdef";
            for (auto byte : version) { signature += hex[byte >> 4]; signature += hex[byte & 15]; }
            bool rebuild = false;
            store_.write([&](sqlite::Database& db) {
                auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_version'");
                rebuild = !state.step() || state.text(0) != signature;
                db.prepare("INSERT INTO app_state(key,value) VALUES('sort_locale',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
                    .bind(1, toUtf8(locale)).run();
            }).get();
            if (rebuild) {
                for (const std::string table : {"tracks", "albums"}) {
                    int64_t after = 0;
                    while (!obsolete()) {
                        size_t count = 0;
                        store_.write([&](sqlite::Database& db) {
                            auto rows = db.prepare("SELECT id FROM " + table + " WHERE id>? ORDER BY id LIMIT 100");
                            rows.bind(1, after);
                            std::vector<int64_t> ids;
                            while (rows.step()) ids.push_back(rows.integer(0));
                            count = ids.size();
                            for (auto id : ids) {
                                if (table == "tracks") updateLibraryTrackText(db, id, locale);
                                else updateLibraryAlbumText(db, id, locale);
                                after = id;
                            }
                        }).get();
                        if (count < 100) break;
                    }
                }
                if (!obsolete()) store_.write([&](sqlite::Database& db) {
                    db.prepare("INSERT INTO app_state(key,value) VALUES('sort_version',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
                        .bind(1, signature).run();
                }).get();
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
        }
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
    }
}

} // namespace camel::server
