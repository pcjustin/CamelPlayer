// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// library.db on local storage with one writer thread (docs/Plan.md §7.6). Opening checks integrity and recovers from a
// damaged file; migrations are backed up first and a failed migration restores the previous database.
#pragma once
#include <condition_variable>
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Server/Library/LibrarySchema.h"
#include "Server/Library/Sqlite.h"
#include "Server/Artwork/ArtworkProvider.h"

namespace camel::server {

class MigrationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The scans in progress (none when idle), summed over roots: files their listings found so far, files they checked,
// and how many have checked everything and are finishing (applying CUE sheets, regrouping albums).
struct LibraryScanProgress {
    int64_t scans = 0;
    int64_t found = 0, checked = 0;
    int64_t finishing = 0;
};

class LibraryStore {
public:
    struct OpenReport {
        bool created = false;        // no database existed
        bool checked = false;        // the previous run did not close it, so its pages were checked
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

    // Finishes the queued writes and closes the database as a clean exit; later writes throw, reads still open.
    void close();

    const OpenReport& report() const { return report_; }
    const std::filesystem::path& path() const { return path_; }

    // Runs `work` on the writer thread inside one transaction; the future rethrows its exception (the transaction is
    // then rolled back).
    std::future<void> write(std::function<void(sqlite::Database&)> work);
    // A read-only connection for the calling thread; WAL lets it read while the writer writes.
    sqlite::Database openReader() const;
    LibraryScanProgress scanProgress() const {
        return {scanning_.load(), scanFound_.load(), scanChecked_.load(), scanFinishing_.load()};
    }

    static constexpr int kKeptBackups = 3;

private:
    friend class LibraryScanner;
    std::atomic<unsigned> activeScans_{0};
    std::atomic<int64_t> scanning_{0}, scanFound_{0}, scanChecked_{0}, scanFinishing_{0};
    // Different roots scan at the same time; scanMutex_ serializes what crosses roots (finishing a scan, CUE edits,
    // absorbing roots), and each root's lock (under jobsMutex_) keeps one scan per root.
    std::mutex scanMutex_;
    std::map<int64_t, std::unique_ptr<std::mutex>> rootLocks_;
    std::mutex jobsMutex_;
    std::multimap<int64_t, CancellationToken> jobs_;
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
