// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

std::filesystem::path canonicalLibraryPath(const std::filesystem::path& path);

struct ScanReport {
    bool complete = false;
    bool deferred = false;
    bool online = true;
    uint32_t rootError = 0; // Win32 error distinguishes access denial from a missing/offline root
    size_t seen = 0, updated = 0, moved = 0;
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
    // The playback callback must be thread-safe; monitor scan workers and fingerprinting may call it concurrently.
    explicit LibraryScanner(LibraryStore& store, std::function<bool()> playbackActive = {},
                            std::function<std::filesystem::path()> playingSource = {})
        : store_(store), playbackActive_(std::move(playbackActive)), playingSource_(std::move(playingSource)) {}
    int64_t addRoot(const std::filesystem::path& path);
    void removeRoot(int64_t root);
    void setCueEncoding(const std::filesystem::path& cuePath, std::optional<uint32_t> codePage);
    // Queues replacing a scan-time length estimate with the exact length playback measured; exact rows are unchanged.
    void recordExactLength(int64_t file, uint32_t sampleRate, uint64_t frames);
    ScanReport scan(int64_t root, const CancellationToken& token = {});
    FingerprintReport fingerprint(int64_t root, const CancellationToken& token = {});

private:
    friend class LibraryMonitor;
    LibraryStore& store_;
    std::function<bool()> playbackActive_;
    std::function<std::filesystem::path()> playingSource_;
    std::function<bool()> playbackGate(const std::filesystem::path& root) const;
};

class LibraryMonitor {
public:
    struct Options {
        std::chrono::milliseconds pollInterval{std::chrono::minutes(30)}; // zero disables local polling only
        std::chrono::milliseconds offlineInterval{std::chrono::seconds(60)};
        std::chrono::milliseconds coalesce{std::chrono::seconds(2)};
        std::function<bool()> playbackActive;
        std::function<std::filesystem::path()> playingSource;
    };
    explicit LibraryMonitor(LibraryStore& store);
    LibraryMonitor(LibraryStore& store, Options options);
    ~LibraryMonitor();
    LibraryMonitor(const LibraryMonitor&) = delete;
    LibraryMonitor& operator=(const LibraryMonitor&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
