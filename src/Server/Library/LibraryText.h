// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace camel::server {
namespace sqlite { class Database; }
class LibraryStore;

std::string normalizeLibrarySearch(std::string_view text);
std::vector<uint8_t> librarySortKey(std::string_view text, std::wstring_view locale = L"zh-TW");
std::vector<uint8_t> libraryNlsVersion(std::wstring_view locale = L"zh-TW");
void updateLibraryTrackText(sqlite::Database& db, int64_t track, std::wstring_view locale = {});
void updateLibraryAlbumText(sqlite::Database& db, int64_t album, std::wstring_view locale = {});

class LibraryTextIndex {
public:
    explicit LibraryTextIndex(LibraryStore& store,
                             std::function<std::vector<uint8_t>(std::wstring_view)> version = libraryNlsVersion);
    ~LibraryTextIndex();
    void setLocale(std::wstring locale);
    void waitIdle();
private:
    void run(std::stop_token stop);
    LibraryStore& store_;
    std::function<std::vector<uint8_t>(std::wstring_view)> version_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::wstring requested_;
    uint64_t generation_ = 1;
    bool pending_ = true, running_ = false;
    std::exception_ptr failure_;
    std::jthread worker_;
};

} // namespace camel::server
