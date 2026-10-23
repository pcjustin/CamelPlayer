// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// library.db on local storage with one writer thread (docs/Plan.md §7.6). Opening checks integrity and recovers from a
// damaged file; migrations are backed up first and a failed migration restores the previous database.
#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Server/Library/LibrarySchema.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {

class MigrationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LibraryStore {
public:
    struct OpenReport {
        bool created = false;        // no database existed
        bool damaged = false;        // the existing file failed its check and was kept as library.db.bad
        bool restoredBackup = false; // ... and the newest backup replaced it
        std::optional<int> migratedFrom;
    };

    // Opens or creates `directory`/library.db, which must be on a local disk. Throws MigrationError (after restoring the
    // database as it was) when a migration fails.
    explicit LibraryStore(const std::filesystem::path& directory, const std::vector<Migration>& migrations = libraryMigrations());
    ~LibraryStore();
    LibraryStore(const LibraryStore&) = delete;
    LibraryStore& operator=(const LibraryStore&) = delete;

    const OpenReport& report() const { return report_; }
    const std::filesystem::path& path() const { return path_; }

    // Runs `work` on the writer thread inside one transaction; the future rethrows its exception (the transaction is
    // then rolled back).
    std::future<void> write(std::function<void(sqlite::Database&)> work);
    // A read-only connection for the calling thread; WAL lets it read while the writer writes.
    sqlite::Database openReader() const;

    static constexpr int kKeptBackups = 3;

private:
    friend class LibraryScanner;
    std::mutex scanMutex_;
    void writerMain();

    std::filesystem::path path_;
    OpenReport report_;
    std::optional<sqlite::Database> db_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::packaged_task<void()>> queue_;
    bool quit_ = false;
    std::thread writer_;
};

// A path on a network share or mapped network drive.
bool isNetworkPath(const std::filesystem::path& path);

} // namespace camel::server
