// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

std::filesystem::path canonicalLibraryPath(const std::filesystem::path& path);
// The library's key for `path` from its spelling alone: canonicalLibraryPath's result for a path the library gave out,
// without opening the file, which costs a network round trip per file on a share.
std::filesystem::path spelledLibraryPath(const std::filesystem::path& path);

struct ScanReport {
    bool complete = false;
    bool deferred = false;
    bool online = true;
    uint32_t rootError = 0; // Win32 error distinguishes access denial from a missing/offline root
    size_t seen = 0, updated = 0, moved = 0;
    size_t deleted = 0; // files missing for 30 days, deleted with what hangs on them
    std::vector<std::string> diagnostics;
};

struct FingerprintReport {
    bool complete = false;
    size_t hashed = 0;
    std::vector<std::string> diagnostics;
};

// Internal Server service; LibraryStore must outlive it. Public browse/command DTOs arrive in M3-R07.
class LibraryScanner {
public:
    // The callbacks must be thread-safe; monitor scan workers and fingerprinting may call them concurrently.
    // `coversShown`: the window reads covers, so work drops to one worker; `playback`: what playback reads now.
    explicit LibraryScanner(LibraryStore& store, std::function<bool()> coversShown = {},
                            std::function<PlaybackReads()> playback = {})
        : store_(store), coversShown_(std::move(coversShown)), playback_(std::move(playback)) {}
    int64_t addRoot(const std::filesystem::path& path);
    // Takes the folder out of the library at once, then queues deleting it (see deleteRemovedRoots); the future is ready
    // once the database is compacted.
    std::future<void> removeRoot(int64_t root);
    // Queues deleting every removed folder with its files and everything that hangs only on them, and compacting
    // library.db afterwards. Favorite artists and composers and artist pictures, kept by name, stay.
    std::future<void> deleteRemovedRoots();
    // Queues replacing a track's length with the exact one playback measured, where the scan estimated it or a header
    // declared another; a row that already holds it is not written.
    void recordExactLength(int64_t file, uint32_t sampleRate, uint64_t frames);
    // With `folders` (inside the root), only those subtrees are listed and reconciled: what a change notification named.
    // A folder that no longer exists just loses its files. The root's last complete scan time is left as it was.
    ScanReport scan(int64_t root, const CancellationToken& token = {}, const std::vector<std::filesystem::path>& folders = {});
    // Records sampled fingerprints of present files that have none or only a full one.
    FingerprintReport fingerprint(int64_t root, const CancellationToken& token = {});

private:
    friend class LibraryMonitor;
    LibraryStore& store_;
    std::function<bool()> coversShown_;
    std::function<PlaybackReads()> playback_;
    std::function<BackgroundPace()> pace(const std::filesystem::path& root) const;
    std::mutex& rootLock(int64_t root);
};

// Settings › Rebuild library: the next complete scan of every root rereads each file's tags and covers,
// keeping file identities (so play statistics and playlists stay) and stored fingerprints.
void requestMetadataReread(LibraryStore& store);
// How many times files left the library (removed folders, files missing for 30 days), for those holding file ids.
int64_t libraryDeletions(sqlite::Database& db);

class LibraryMonitor {
public:
    struct Options {
        std::chrono::milliseconds pollInterval{std::chrono::minutes(30)};     // local roots; zero disables polling
        std::chrono::milliseconds networkPollInterval{std::chrono::hours(6)}; // shares; zero disables polling
        std::chrono::milliseconds offlineInterval{std::chrono::seconds(60)};
        std::chrono::milliseconds coalesce{std::chrono::seconds(2)};
        std::function<bool()> coversShown;
        std::function<PlaybackReads()> playback;
        bool watchChanges = true; // off in tests, standing for a share whose change notifications are missed
        // On in tests, standing for storage that refuses change notifications (some NAS and SMB servers, rclone, WebDAV).
        bool refuseNotifications = false;
    };
    explicit LibraryMonitor(LibraryStore& store);
    LibraryMonitor(LibraryStore& store, const Options& options);
    ~LibraryMonitor();
    LibraryMonitor(const LibraryMonitor&) = delete;
    LibraryMonitor& operator=(const LibraryMonitor&) = delete;
    // Rereads the registered roots on the next pass instead of after the availability interval.
    void refreshRoots();
    // Scans every online root now, at High as the user asked for it.
    void rescanAll();
    // Scans a root the user just added, at High.
    void scanAdded(int64_t root);
    // Scan now and Scan this folder: every online root, or `folder` of `root` and everything below it, finding only what
    // changed, at High as the user asked for it; a root whose scan runs already is left to it.
    void scanAll();
    void scanFolder(int64_t root, const std::filesystem::path& folder);
    // Stops scanning without waiting, which the destructor does.
    void cancel();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
