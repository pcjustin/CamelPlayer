// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include "Server/Library/LibraryBrowse.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Library/LibraryText.h"
#include "TestUtil.h"

using namespace camel::server;
using Clock = std::chrono::steady_clock;

TEST(LibraryBrowse, PagesSearchesCancelsAndRejectsStaleRevisions) {
    const auto directory = testutil::tempDir() / "library-browse-pages";
    std::filesystem::remove_all(directory);
    LibraryStore store(directory);
    const auto emptyRevision = browseTracks(store, 0, 1).revision;
    store.write([](sqlite::Database& db) {
        db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/music',0);"
                "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/music');"
                "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) "
                "VALUES(1,1,1,'C:/music/one.flac','audio',1,1);"
                "INSERT INTO tracks(id,file_id,title,title_norm,title_sort,artist,album) "
                "VALUES(1,1,'Café orchestra','café orchestra',X'01','Artist','Album');");
    }).get();
    EXPECT_EQ(emptyRevision, 0u);
    EXPECT_THROW(browseTracks(store, 0, 1, emptyRevision), std::runtime_error);
    auto first = browseTracks(store, 0, 1);
    ASSERT_EQ(first.total, 1);
    ASSERT_EQ(first.tracks.size(), 1u);
    EXPECT_EQ(first.tracks.front().title, "Café orchestra");
    EXPECT_TRUE(browseTracks(store, 1, 1, first.revision).tracks.empty());
    EXPECT_EQ(searchTracks(store, "Café orchestra", 0, 10, first.revision).total, 1);
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_THROW(browseTracks(store, 0, 1, 0, cancelled), std::runtime_error);
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='Changed'"); }).get();
    EXPECT_THROW(browseTracks(store, 0, 1, first.revision), std::runtime_error);
    EXPECT_THROW(browseTracks(store, -1, 1), std::invalid_argument);
    EXPECT_THROW(browseTracks(store, 0, 501), std::invalid_argument);
}

TEST(LibraryBrowse, WarmFirstPageMeetsOneHundredMillisecondTargetAtOneHundredThousandTracks) {
    const auto directory = testutil::tempDir() / "library-browse-100k";
    std::filesystem::remove_all(directory);
    LibraryStore store(directory);
    store.write([](sqlite::Database& db) {
        db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/corpus',0);"
                "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/corpus');"
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) "
                "SELECT x,1,1,'C:/corpus/'||x||'.flac','audio',100,1 FROM n;"
                "INSERT INTO tracks(id,file_id,title,title_norm,album,album_norm,artist,artist_norm,title_sort) "
                "SELECT id,id,printf('Track %06d Café Orchestra',id),printf('track %06d café orchestra',id),"
                "'Test Album','test album','Test Artist','test artist',printf('%06d',id) FROM files;");
    }).get();

    const auto queryStart = Clock::now();
    for (int i = 0; i < 30; ++i) normalizeLibrarySearch("099999 Café Orchestra");
    const double normalizeAverageMs = std::chrono::duration<double, std::milli>(Clock::now() - queryStart).count() / 30.0;
    for (int i = 0; i < 8; ++i) ASSERT_EQ(searchTracks(store, "099999 Café Orchestra", 0, 50).tracks.size(), 1u);
    std::vector<double> samples;
    samples.reserve(30);
    FILETIME creation{}, exit{}, kernelStart{}, userStart{}, kernelEnd{}, userEnd{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelStart, &userStart);
    for (int i = 0; i < 30; ++i) {
        const auto start = Clock::now();
        auto page = searchTracks(store, "099999 Café Orchestra", 0, 50);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        ASSERT_EQ(page.total, 1);
        ASSERT_EQ(page.tracks.front().id, 99999);
    }
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelEnd, &userEnd);
    std::sort(samples.begin(), samples.end());
    const double p95 = samples[size_t(samples.size() * 0.95)];
    auto db = store.openReader();
    auto databaseBytes = std::filesystem::file_size(store.path());
    for (const auto* suffix : {L"-wal", L"-shm"}) {
        std::error_code ec;
        databaseBytes += std::filesystem::file_size(std::filesystem::path(store.path().wstring() + suffix), ec);
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    auto ticks = [](FILETIME value) {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    };
    const auto cpu100ns = ticks(kernelEnd) - ticks(kernelStart) + ticks(userEnd) - ticks(userStart);
    RecordProperty("corpus_tracks", 100000);
    RecordProperty("query_runs", samples.size());
    RecordProperty("warm_search_first_page_p95_ms", p95);
    RecordProperty("normalization_average_ms", normalizeAverageMs);
    RecordProperty("database_bytes", databaseBytes);
    RecordProperty("processor_count", system.dwNumberOfProcessors);
    RecordProperty("physical_memory_bytes", memory.ullTotalPhys);
    RecordProperty("query_cpu_delta_100ns", cpu100ns);
    EXPECT_LT(p95, 100.0);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM library_tracks"), 100000);
}
