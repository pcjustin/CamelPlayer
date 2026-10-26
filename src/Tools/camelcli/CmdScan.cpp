// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// scan-bench: times building a library from one folder, such as a NAS folder, as the player builds it, without touching
// the player's own library.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "Commands.h"
#include "Common/util/Text.h"
#include "Server/Artwork/ArtworkCache.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Library/LibraryText.h"

namespace camelcli {

// scan-bench <folder> [--data <dir>]: builds a fresh library of <folder> in <dir> (default %TEMP%\camelcli-scan-bench)
// the way the player does: scan, fingerprints, cover thumbnails and search text; then scans it again unchanged, as
// every start of the player does.
int cmdScanBench(const Args& args) {
    const auto rest = positional(args);
    if (rest.empty()) {
        std::puts("usage: scan-bench <folder> [--data <dir>]");
        return 2;
    }
    namespace fs = std::filesystem;
    const fs::path folder = rest[0];
    const fs::path data = option(args, L"--data").value_or((fs::temp_directory_path() / L"camelcli-scan-bench").wstring());
    std::error_code ec;
    fs::remove_all(data, ec);
    const auto started = std::chrono::steady_clock::now();
    camel::server::LibraryStore store(data);
    camel::server::LibraryTextIndex text(store);
    camel::server::LibraryScanner scanner(store);
    camel::server::ArtworkCache artwork(data / L"thumbs");
    const auto root = scanner.addRoot(folder);
    const auto seconds = [](auto since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    };
    const auto step = [&](const char* what, auto&& work) {
        const auto at = std::chrono::steady_clock::now();
        const std::string result = work();
        std::printf("%-12s %7.2f s  %s\n", what, seconds(at), result.c_str());
        std::fflush(stdout);
    };
    step("scan", [&] {
        const auto report = scanner.scan(root);
        auto db = store.openReader();
        return "complete=" + std::to_string(report.complete) + ", " +
               std::to_string(db.scalar("SELECT COUNT(*) FROM files WHERE available=1").value_or(0)) + " files, " +
               std::to_string(db.scalar("SELECT COUNT(*) FROM tracks").value_or(0)) + " tracks, " +
               std::to_string(db.scalar("SELECT COUNT(*) FROM albums").value_or(0)) + " albums, " +
               std::to_string(db.scalar("SELECT COUNT(*) FROM albums WHERE cover_id IS NOT NULL").value_or(0)) + " with covers";
    });
    step("fingerprint", [&] {
        const auto report = scanner.fingerprint(root);
        return "complete=" + std::to_string(report.complete) + ", " + std::to_string(report.hashed) + " files";
    });
    step("thumbnails", [&] { return std::to_string(artwork.prepare(store, {})) + " album covers"; });
    step("search text", [&] {
        text.waitIdle();
        return std::string("ready");
    });
    std::printf("%-12s %7.2f s\n", "total", seconds(started));
    step("rescan", [&] { return "complete=" + std::to_string(scanner.scan(root).complete); });
    return 0;
}

} // namespace camelcli
