// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <fstream>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>
#include <taglib/flacfile.h>
#include <taglib/tag.h>

#include "Common/util/Text.h"
#include "Server/Library/LibraryScanner.h"
#include "TestUtil.h"

using namespace camel::server;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

namespace {

fs::path nasRoot() {
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CAMEL_NAS_ROOT", root, DWORD(std::size(root)));
    if (!length || length >= std::size(root)) throw std::runtime_error("set CAMEL_NAS_ROOT to an authorized NAS share");
    fs::path path(root);
    if (!path.is_absolute() || !isNetworkPath(path)) throw std::runtime_error("CAMEL_NAS_ROOT must be a network path");
    return path;
}

fs::path localRun(const char* name) {
    const auto path = testutil::tempDir() / "nas-library" /
                      (std::string(name) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directories(path);
    return path;
}

int64_t count(LibraryStore& store, const char* sql = "SELECT COUNT(*) FROM files WHERE available=1") {
    return store.openReader().scalar(sql).value();
}

bool waitFor(const std::function<bool()>& check) {
    const auto end = Clock::now() + std::chrono::seconds(120);
    do {
        if (check()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (Clock::now() < end);
    return false;
}

Json scan(LibraryStore& store, LibraryScanner& scanner, int64_t root, const char* phase) {
    const auto start = Clock::now();
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root); });
    while (job.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        std::cout << phase << ": " << count(store) << " published files, "
                  << std::chrono::duration<double>(Clock::now() - start).count() << " s" << std::endl;
    const auto result = job.get();
    Json out{{"complete", result.complete},
             {"online", result.online},
             {"root_error", result.rootError},
             {"seen", result.seen},
             {"updated", result.updated},
             {"diagnostics", result.diagnostics},
             {"seconds", std::chrono::duration<double>(Clock::now() - start).count()}};
    auto summary = out;
    summary["diagnostics"] = result.diagnostics.size();
    std::cout << phase << ": " << summary.dump() << std::endl;
    return out;
}

} // namespace

TEST(LibraryNas, DISABLED_ReadOnlyFullAndIncrementalScan) {
    const auto root = nasRoot();
    const auto local = localRun("read-only");
    std::cout << "Local results: " << camel::toUtf8(local.wstring()) << std::endl;
    LibraryStore store(local);
    LibraryScanner scanner(store);
    const auto id = scanner.addRoot(root);
    ASSERT_EQ(scanner.addRoot(root / L"."), id);
    Json report;
    report["full"] = scan(store, scanner, id, "full");
    report["files"] = count(store);
    report["tracks"] = count(store, "SELECT COUNT(*) FROM tracks");
    auto db = store.openReader();
    auto formats = db.prepare("SELECT format,COUNT(*) FROM tracks GROUP BY format");
    while (formats.step()) report["formats"][formats.text(0)] = formats.integer(1);
    report["incremental"] = scan(store, scanner, id, "incremental");
    report["files_after_incremental"] = count(store);
    CancellationToken token;
    token.cancel();
    EXPECT_FALSE(scanner.scan(id, token).complete);
    EXPECT_EQ(count(store), report["files_after_incremental"].get<int64_t>());
    std::ofstream(local / "report.json") << report.dump(2);
    std::cout << "Summary: " << report["formats"].dump() << ", " << report["tracks"] << " tracks" << std::endl;
    EXPECT_TRUE(report["full"]["online"].get<bool>());
    EXPECT_TRUE(report["full"]["complete"].get<bool>());
    EXPECT_GT(report["tracks"].get<int64_t>(), 0);
    EXPECT_TRUE(report["incremental"]["complete"].get<bool>());
    EXPECT_EQ(report["incremental"]["updated"].get<size_t>(), 0);
    EXPECT_EQ(report["files"], report["files_after_incremental"]);
}

TEST(LibraryNas, DISABLED_IsolatedChangesOverflowAndReconnection) {
    const auto share = nasRoot();
    const auto name =
        L".camelplayer-nas-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto root = share / name;
    const auto offline = share / (name + L"-offline");
    ASSERT_EQ(root.parent_path(), share);
    ASSERT_FALSE(fs::exists(root));
    ASSERT_FALSE(fs::exists(offline));
    ASSERT_TRUE(fs::create_directory(root));
    std::vector<fs::path> created{root, root / L"監控", root / "one.flac", root / L"監控" / "renamed.flac"};
    auto cleanup = wil::scope_exit([&] {
        std::error_code error;
        if (fs::exists(offline, error) && !fs::exists(root, error)) fs::rename(offline, root, error);
        for (auto p = created.rbegin(); p != created.rend(); ++p) fs::remove(*p, error);
        if (fs::exists(root, error) || fs::exists(offline, error))
            std::cerr << "NAS fixture cleanup incomplete: " << camel::toUtf8(root.wstring()) << std::endl;
    });
    fs::create_directory(root / L"監控");
    const auto local = localRun("changes");
    std::cout << "Local results: " << camel::toUtf8(local.wstring()) << std::endl;
    LibraryStore store(local);
    LibraryScanner scanner(store);
    scanner.addRoot(root);
    LibraryMonitor::Options options;
    options.offlineInterval = std::chrono::milliseconds(200);
    options.pollInterval = std::chrono::seconds(2);
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL") == 1; }));
    fs::copy_file(testutil::testFile("16-44.1k"), root / "one.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    std::cout << "NAS create detection (notifications with 2 s polling fallback): passed" << std::endl;
    {
        TagLib::FLAC::File tags((root / "one.flac").c_str());
        tags.tag()->setTitle("NAS updated title");
        ASSERT_TRUE(tags.save());
    }
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM tracks WHERE title='NAS updated title'") == 1; }));
    std::cout << "NAS metadata refresh (notifications with 2 s polling fallback): passed" << std::endl;
    fs::rename(root / "one.flac", root / L"監控" / "renamed.flac");
    ASSERT_TRUE(waitFor([&] {
        return count(store, "SELECT COUNT(*) FROM files WHERE available=1 AND path LIKE '%renamed.flac'") == 1 &&
               count(store) == 1;
    }));
    std::cout << "NAS recursive rename detection: passed" << std::endl;

    wil::unique_handle directory(CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    ASSERT_TRUE(directory);
    wil::unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event);
    OVERLAPPED io{};
    io.hEvent = event.get();
    alignas(DWORD) std::array<uint8_t, 32768> buffer{};
    auto cancel = wil::scope_exit([&] {
        CancelIoEx(directory.get(), &io);
        DWORD bytes = 0;
        GetOverlappedResult(directory.get(), &io, &bytes, TRUE);
    });
    const auto arm = [&] {
        ResetEvent(event.get());
        io = {};
        io.hEvent = event.get();
        return bool(ReadDirectoryChangesW(directory.get(), buffer.data(), DWORD(buffer.size()), TRUE,
                                          FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &io, nullptr));
    };
    ASSERT_TRUE(arm());
    created.push_back(root / "event-0.lrc");
    std::ofstream(created.back()) << "words";
    ASSERT_EQ(WaitForSingleObject(event.get(), 10000), WAIT_OBJECT_0);
    DWORD bytes = 0;
    ASSERT_TRUE(GetOverlappedResult(directory.get(), &io, &bytes, FALSE));
    ASSERT_GT(bytes, 0);
    constexpr int kEvents = 2000;
    for (int i = 1; i < kEvents; i++) {
        created.push_back(root / ("event-" + std::to_string(i) + ".lrc"));
        std::ofstream file(created.back());
        ASSERT_TRUE(file);
        file << "words";
    }
    ASSERT_TRUE(arm());
    ASSERT_EQ(WaitForSingleObject(event.get(), 10000), WAIT_OBJECT_0);
    const bool ok = bool(GetOverlappedResult(directory.get(), &io, &bytes, FALSE));
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    EXPECT_TRUE((ok && bytes == 0) || error == ERROR_NOTIFY_ENUM_DIR) << "bytes=" << bytes << " error=" << error;
    ASSERT_TRUE(waitFor([&] { return count(store) == kEvents + 1; }));
    std::cout << "NAS notification overflow and monitor convergence: passed" << std::endl;
    fs::rename(root, offline);
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots") == 0; }));
    EXPECT_EQ(count(store), kEvents + 1);
    fs::rename(offline, root);
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots") == 1; }));
    EXPECT_EQ(count(store), kEvents + 1);
    std::cout << "NAS directory unavailability and reconnection: passed" << std::endl;
    fs::remove(root / L"監控" / "renamed.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == kEvents; }));
    std::cout << "NAS delete detection: passed" << std::endl;
    std::ofstream(local / "report.json") << Json{{"events", kEvents},       {"overflow_bytes", bytes},
                                                 {"overflow_error", error}, {"available_files", count(store)},
                                                 {"poll_seconds", 2},       {"passed", !::testing::Test::HasFailure()}}
                                                .dump(2);
}
