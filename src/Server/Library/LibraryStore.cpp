// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibraryStore.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <utility>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

#pragma comment(lib, "shlwapi.lib")

namespace camel::server {
namespace {

std::filesystem::path withSuffix(const std::filesystem::path& p, const wchar_t* suffix) {
    return std::filesystem::path(p).concat(suffix);
}

// The database file and its WAL companions.
void removeDatabase(const std::filesystem::path& db) {
    std::error_code ec;
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) std::filesystem::remove(withSuffix(db, suffix), ec);
}

bool healthy(sqlite::Database& db) {
    auto check = db.prepare("PRAGMA quick_check");
    return check.step() && check.text(0) == "ok";
}

std::optional<sqlite::Database> openIfHealthy(const std::filesystem::path& path) {
    try {
        sqlite::Database db(path);
        if (healthy(db)) return db;
    } catch (const sqlite::Error& e) {
        log::warn("library: cannot open %s: %s", toUtf8(path.wstring()).c_str(), e.what());
    }
    return std::nullopt;
}

// library.db.v<N>.bak files, newest schema first.
std::vector<std::pair<int, std::filesystem::path>> backups(const std::filesystem::path& db) {
    std::vector<std::pair<int, std::filesystem::path>> out;
    std::error_code ec;
    const std::wstring prefix = db.filename().wstring() + L".v";
    for (const auto& entry : std::filesystem::directory_iterator(db.parent_path(), ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, L".bak") != 0)
            continue;
        const std::wstring number = name.substr(prefix.size(), name.size() - prefix.size() - 4);
        if (number.empty() || number.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
        out.emplace_back(std::stoi(number), entry.path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return out;
}

void configure(sqlite::Database& db) {
    db.exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;");
}

} // namespace

bool isNetworkPath(const std::filesystem::path& path) {
    const std::wstring text = std::filesystem::absolute(path).wstring();
    if (_wcsnicmp(text.c_str(), L"\\\\?\\UNC\\", 8) == 0) return true;
    if (PathIsNetworkPathW(text.c_str())) return true;
    const std::wstring root = std::filesystem::absolute(path).root_path().wstring();
    return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

LibraryStore::LibraryStore(const std::filesystem::path& directory, const std::vector<Migration>& migrations)
    : path_(directory / L"library.db") {
    if (isNetworkPath(directory))
        throw std::runtime_error("library.db must be on a local disk, not " + toUtf8(directory.wstring()));
    std::filesystem::create_directories(directory);

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        report_.created = true;
        db_.emplace(path_);
    } else if (auto db = openIfHealthy(path_)) {
        db_.emplace(std::move(*db));
    } else {
        report_.damaged = true;
        const auto bad = withSuffix(path_, L".bad");
        removeDatabase(bad);
        std::filesystem::rename(path_, bad, ec);
        removeDatabase(path_);
        for (const auto& [version, backup] : backups(path_)) {
            std::filesystem::copy_file(backup, path_, std::filesystem::copy_options::overwrite_existing, ec);
            auto restored = ec ? std::nullopt : openIfHealthy(path_);
            if (restored) {
                db_.emplace(std::move(*restored));
                report_.restoredBackup = true;
                log::warn("library: damaged database replaced by the schema %d backup", version);
                break;
            }
            removeDatabase(path_);
        }
        if (!db_) {
            log::warn("library: damaged database replaced by an empty one; the library is rebuilt by scanning");
            db_.emplace(path_);
        }
    }
    configure(*db_);

    const int version = db_->userVersion();
    if (version > int(migrations.size()))
        throw MigrationError("library.db has schema version " + std::to_string(version) + ", newer than this program (" +
                             std::to_string(migrations.size()) + ")");
    if (version < int(migrations.size())) {
        std::optional<std::filesystem::path> backup;
        if (version > 0) {
            backup = withSuffix(path_, (L".v" + std::to_wstring(version) + L".bak").c_str());
            db_->backupTo(*backup);
            const auto all = backups(path_);
            for (size_t i = kKeptBackups; i < all.size(); i++) std::filesystem::remove(all[i].second, ec);
        }
        try {
            migrate(*db_, migrations);
            report_.migratedFrom = version;
        } catch (const std::exception& e) {
            db_.reset();
            removeDatabase(path_);
            if (backup) std::filesystem::copy_file(*backup, path_, std::filesystem::copy_options::overwrite_existing);
            throw MigrationError("library.db migration from schema " + std::to_string(version) + " failed (" + e.what() +
                                 "); the previous database was kept");
        }
    }
    writer_ = std::thread([this] { writerMain(); });
}

LibraryStore::~LibraryStore() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
}

std::future<void> LibraryStore::write(std::function<void(sqlite::Database&)> work) {
    std::packaged_task<void()> task([this, work = std::move(work)] {
        sqlite::Transaction t(*db_);
        work(*db_);
        t.commit();
    });
    auto done = task.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_all();
    return done;
}

sqlite::Database LibraryStore::openReader() const {
    sqlite::Database reader(path_, sqlite::Database::Mode::ReadOnly);
    reader.exec("PRAGMA query_only = ON");
    return reader;
}

void LibraryStore::writerMain() {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (queue_.empty()) return; // quitting with nothing left to write
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

} // namespace camel::server
