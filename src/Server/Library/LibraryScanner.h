// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

struct ScanReport {
    bool complete = false;
    bool online = true;
    uint32_t rootError = 0; // Win32 error distinguishes access denial from a missing/offline root
    size_t seen = 0, updated = 0;
    std::vector<std::string> diagnostics;
};

// Internal Server service; LibraryStore must outlive it. Public browse/command DTOs arrive in M3-R07.
class LibraryScanner {
public:
    explicit LibraryScanner(LibraryStore& store) : store_(store) {}
    int64_t addRoot(const std::filesystem::path& path);
    ScanReport scan(int64_t root, const CancellationToken& token = {});

private:
    LibraryStore& store_;
};

class LibraryMonitor {
public:
    struct Options {
        std::chrono::milliseconds pollInterval{std::chrono::minutes(30)}; // zero disables local polling only
        std::chrono::milliseconds offlineInterval{std::chrono::seconds(60)};
        std::chrono::milliseconds coalesce{std::chrono::seconds(2)};
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
