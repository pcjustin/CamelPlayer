// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <winnetwk.h>

#include <fstream>
#include <algorithm>
#include <iostream>
#include <optional>
#include <thread>
#include <psapi.h>

#include <nlohmann/json.hpp>
#include <taglib/flacfile.h>
#include <taglib/tag.h>

#include "Common/util/Text.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryIdentity.h"
#include "Server/Playback/LocalServer.h"
#include "TestUtil.h"

using namespace camel::server;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

#pragma comment(lib, "psapi.lib")

namespace {

fs::path nasRoot() {
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CAMEL_NAS_ROOT", root, DWORD(std::size(root)));
    if (!length || length >= std::size(root)) throw std::runtime_error("set CAMEL_NAS_ROOT to an authorized NAS share");
    fs::path path(root);
    if (!path.is_absolute() || !isNetworkPath(path)) throw std::runtime_error("CAMEL_NAS_ROOT must be a network path");
    return path;
}

fs::path nasIndex() {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CAMEL_NAS_INDEX", path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) throw std::runtime_error("set CAMEL_NAS_INDEX to an existing local library index");
    return fs::path(path);
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

uint64_t fileBytes(const fs::path& path) {
    uint64_t bytes = 0;
    for (const auto* suffix : {L"", L"-wal", L"-shm"}) {
        std::error_code error;
        const auto size = fs::file_size(fs::path(path.wstring() + suffix), error);
        if (!error) bytes += size;
    }
    return bytes;
}

uint64_t cpuTicks(FILETIME kernel, FILETIME user) {
    auto ticks = [](FILETIME value) {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    };
    return ticks(kernel) + ticks(user);
}

Json machine() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
#ifdef _DEBUG
    constexpr const char* configuration = "Debug";
#else
    constexpr const char* configuration = "Release";
#endif
    return {{"build_configuration", configuration}, {"processor_count", system.dwNumberOfProcessors},
            {"physical_memory_bytes", memory.ullTotalPhys}, {"available_memory_bytes", memory.ullAvailPhys}};
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
    const auto generation = count(store, "SELECT COALESCE(MAX(last_seen),0) FROM files");
    FILETIME creation{}, exit{}, kernelStart{}, userStart{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelStart, &userStart);
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root); });
    std::optional<double> firstBatchSeconds;
    int64_t firstBatchFiles = 0;
    auto nextReport = start + std::chrono::seconds(5);
    while (job.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
        if (!firstBatchSeconds) {
            const auto sql = "SELECT COUNT(*) FROM files WHERE root_id=" + std::to_string(root) +
                             " AND last_seen>" + std::to_string(generation);
            firstBatchFiles = count(store, sql.c_str());
            if (firstBatchFiles > 0) firstBatchSeconds = std::chrono::duration<double>(Clock::now() - start).count();
        }
        if (Clock::now() >= nextReport) {
            std::cout << phase << ": " << count(store) << " published files, "
                      << std::chrono::duration<double>(Clock::now() - start).count() << " s" << std::endl;
            nextReport = Clock::now() + std::chrono::seconds(5);
        }
    }
    const auto result = job.get();
    FILETIME kernelEnd{}, userEnd{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelEnd, &userEnd);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    Json diagnostics = Json::array();
    for (size_t i = 0; i < std::min<size_t>(result.diagnostics.size(), 20); ++i)
        diagnostics.push_back(result.diagnostics[i]);
    Json out{{"complete", result.complete},
             {"online", result.online},
             {"root_error", result.rootError},
             {"seen", result.seen},
             {"updated", result.updated},
             {"diagnostic_count", result.diagnostics.size()},
             {"diagnostics_sample", diagnostics},
             {"first_batch_seconds", firstBatchSeconds ? Json(*firstBatchSeconds) : Json(nullptr)},
             {"first_batch_files", firstBatchFiles},
             {"seconds", std::chrono::duration<double>(Clock::now() - start).count()},
             {"process_cpu_seconds", double(cpuTicks(kernelEnd, userEnd) - cpuTicks(kernelStart, userStart)) / 1e7},
             {"process_working_set_bytes", memory.WorkingSetSize},
             {"database_bytes", fileBytes(store.path())},
             {"cache_condition", std::string(phase).find("full") != std::string::npos
                                     ? "fresh local index; Windows and NAS server caches were not flushed"
                                     : "same process and SMB session after full scan"}};
    std::cout << phase << ": " << out.dump() << std::endl;
    return out;
}

Json fingerprint(LibraryStore& store, LibraryScanner& scanner, int64_t root) {
    const auto start = Clock::now();
    FILETIME creation{}, exit{}, kernelStart{}, userStart{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelStart, &userStart);
    auto job = std::async(std::launch::async, [&] { return scanner.fingerprint(root); });
    const auto sql = "SELECT COUNT(*) FROM files WHERE root_id=" + std::to_string(root) + " AND content_hash IS NOT NULL";
    while (job.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        std::cout << "fingerprint: " << count(store, sql.c_str()) << " hashes saved, "
                  << std::chrono::duration<double>(Clock::now() - start).count() << " s" << std::endl;
    const auto result = job.get();
    FILETIME kernelEnd{}, userEnd{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelEnd, &userEnd);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    Json diagnostics = Json::array();
    for (size_t i = 0; i < std::min<size_t>(result.diagnostics.size(), 20); ++i)
        diagnostics.push_back(result.diagnostics[i]);
    Json out{{"complete", result.complete},
             {"hashed", result.hashed},
             {"diagnostic_count", result.diagnostics.size()},
             {"diagnostics_sample", diagnostics},
             {"seconds", std::chrono::duration<double>(Clock::now() - start).count()},
             {"process_cpu_seconds", double(cpuTicks(kernelEnd, userEnd) - cpuTicks(kernelStart, userStart)) / 1e7},
             {"process_working_set_bytes", memory.WorkingSetSize},
             {"database_bytes", fileBytes(store.path())},
             {"cache_condition", "same process and SMB session after full and incremental scans"}};
    std::cout << "fingerprint: " << out.dump() << std::endl;
    return out;
}

} // namespace

TEST(LibraryNas, DISABLED_UniqueFingerprintsPreserveMoveHistory) {
    const auto root = nasRoot();
    const auto fixture = root / (L".camelplayer-nas-test-identity-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64()));
    ASSERT_TRUE(fs::create_directory(fixture));
    const auto old = fixture / "old.flac", renamed = fixture / "renamed.flac", copy = fixture / "copy.flac";
    const auto one = fixture / "one.flac", two = fixture / "two.flac";
    auto cleanup = wil::scope_exit([&] {
        std::error_code error;
        for (const auto& path : {old, renamed, copy, one, two}) fs::remove(path, error);
        fs::remove(fixture, error);
    });
    fs::copy_file(testutil::testFile("16-44.1k"), old);
    ASSERT_TRUE(library::fileVersion(old).volumeKey.empty());
    const auto local = localRun("identity");
    std::cout << "Local identity results: " << camel::toUtf8(local.wstring()) << std::endl;
    LibraryStore store(local);
    LibraryScanner scanner(store);
    const auto id = scanner.addRoot(fixture);
    ASSERT_TRUE(scanner.scan(id).complete);
    const auto original = count(store, "SELECT id FROM files WHERE available=1");
    const auto track = count(store, "SELECT id FROM tracks");
    store
        .write([=](sqlite::Database& db) {
            db.prepare("INSERT INTO stats(file_id,rating,favorite,play_count) VALUES(?,5,1,17)").bind(1, original).run();
        })
        .get();
    ASSERT_EQ(scanner.fingerprint(id).hashed, 1);
    fs::rename(old, renamed);
    EXPECT_EQ(scanner.scan(id).moved, 1);
    EXPECT_EQ(count(store, "SELECT id FROM files WHERE available=1"), original);
    EXPECT_EQ(count(store, "SELECT rating FROM stats"), 5);
    EXPECT_EQ(count(store, "SELECT id FROM tracks WHERE file_id IN (SELECT file_id FROM stats)"), track);
    fs::rename(renamed, old);
    EXPECT_EQ(scanner.scan(id).moved, 1);
    EXPECT_EQ(count(store, "SELECT id FROM files WHERE available=1"), original);
    fs::rename(old, renamed);
    EXPECT_EQ(scanner.scan(id).moved, 1);
    fs::copy_file(renamed, copy);
    EXPECT_EQ(scanner.scan(id).moved, 0);
    EXPECT_EQ(count(store), 2);
    fs::remove(copy);
    ASSERT_TRUE(scanner.scan(id).complete);
    // A retained copy fingerprint makes this later move ambiguous; it must not acquire the original history.
    fs::rename(renamed, one);
    fs::copy_file(one, two);
    EXPECT_EQ(scanner.scan(id).moved, 0);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE available=1 AND id IN (SELECT file_id FROM stats)"), 0);
    Json report{{"original_id", original},
                {"rating", count(store, "SELECT rating FROM stats")},
                {"play_count", count(store, "SELECT play_count FROM stats")},
                {"available", count(store)},
                {"trusted_nas_ids", count(store, "SELECT COUNT(*) FROM files WHERE volume_key<>''")}};
    std::ofstream(local / "report.json") << report.dump(2);
}

TEST(LibraryNas, DISABLED_ReadOnlyFullAndIncrementalScan) {
    const auto root = nasRoot();
    const auto local = localRun("read-only");
    std::cout << "Local results: " << camel::toUtf8(local.wstring()) << std::endl;
    LibraryStore store(local);
    LibraryScanner scanner(store);
    const auto id = scanner.addRoot(root);
    ASSERT_EQ(scanner.addRoot(root / L"."), id);
    Json report{{"environment", machine()},
                {"root", camel::toUtf8(root.wstring())},
                {"local_index", camel::toUtf8(local.wstring())}};
    report["full"] = scan(store, scanner, id, "full");
    report["files"] = count(store);
    report["tracks"] = count(store, "SELECT COUNT(*) FROM tracks");
    auto db = store.openReader();
    auto formats = db.prepare("SELECT format,COUNT(*) FROM tracks GROUP BY format");
    while (formats.step()) report["formats"][formats.text(0)] = formats.integer(1);
    report["incremental"] = scan(store, scanner, id, "incremental");
    report["files_after_incremental"] = count(store);
    report["fingerprint"] = fingerprint(store, scanner, id);
    CancellationToken token;
    token.cancel();
    EXPECT_FALSE(scanner.scan(id, token).complete);
    EXPECT_EQ(count(store), report["files_after_incremental"].get<int64_t>());
    std::ofstream(local / "report.json") << report.dump(2);
    std::cout << "Summary: " << report["formats"].dump() << ", " << report["tracks"] << " tracks" << std::endl;
    EXPECT_TRUE(report["full"]["online"].get<bool>());
    EXPECT_GT(report["full"]["seen"].get<int64_t>(), 0);
    EXPECT_GT(report["tracks"].get<int64_t>(), 0);
    EXPECT_TRUE(report["incremental"]["online"].get<bool>());
    EXPECT_GT(report["incremental"]["seen"].get<int64_t>(), 0);
    EXPECT_EQ(report["incremental"]["updated"].get<size_t>(), 0);
    EXPECT_EQ(report["files"], report["files_after_incremental"]);
}

TEST(LibraryNas, DISABLED_FingerprintExistingLocalIndex) {
    const auto local = nasIndex();
    LibraryStore store(local);
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(nasRoot());
    auto report = fingerprint(store, scanner, root);
    report["root"] = camel::toUtf8(nasRoot().wstring());
    std::ofstream(local / "fingerprint-report.json") << report.dump(2);
    EXPECT_GT(report["hashed"].get<int64_t>(), 0);
}

TEST(LibraryNas, DISABLED_LocalSsdFirstFullScanOfOneHundredThousandTracks) {
    constexpr int64_t trackCount = 100000;
    constexpr int64_t linksPerSource = 1022;
    const auto local = localRun("local-100k");
    const auto music = local / "music";
    const auto sources = local / "sources";
    fs::create_directories(music);
    fs::create_directories(sources);
    auto cleanup = wil::scope_exit([&] {
        std::error_code error;
        fs::remove_all(music, error);
        fs::remove_all(sources, error);
    });
    const auto fixture = testutil::testFile("16-44.1k");
    const int64_t sourceCount = (trackCount + linksPerSource - 1) / linksPerSource;
    for (int64_t i = 0; i < sourceCount; ++i)
        fs::copy_file(fixture, sources / (std::to_string(i) + ".flac"));

    const auto setupStart = Clock::now();
    for (int64_t i = 0; i < trackCount; ++i) {
        const auto source = sources / (std::to_string(i / linksPerSource) + ".flac");
        const auto destination = music / (std::to_string(i) + ".flac");
        ASSERT_TRUE(CreateHardLinkW(destination.c_str(), source.c_str(), nullptr)) << GetLastError();
    }
    const double setupSeconds = std::chrono::duration<double>(Clock::now() - setupStart).count();

    Json report{{"environment", machine()},
                {"corpus_tracks", trackCount},
                {"distinct_payload_files", sourceCount},
                {"cache_condition", "fresh local index; 100,000 paths hard-link to 98 copied FLAC payloads"},
                {"fixture_setup_seconds", setupSeconds},
                {"music_root", camel::toUtf8(music.wstring())}};
    {
        LibraryStore store(local / "index");
        LibraryScanner scanner(store);
        const auto id = scanner.addRoot(music);
        report["full"] = scan(store, scanner, id, "local-full");
        report["tracks"] = count(store, "SELECT COUNT(*) FROM library_tracks");
        report["database_bytes"] = fileBytes(store.path());
        std::ofstream(local / "report.json") << report.dump(2);
        EXPECT_TRUE(report["full"]["complete"].get<bool>());
        EXPECT_EQ(report["tracks"], trackCount);
        EXPECT_LE(report["full"]["seconds"].get<double>(), 900.0);
    }
    std::cout << "Local 100k scan report: " << camel::toUtf8(local.wstring()) << std::endl;
}

TEST(LibraryNas, DISABLED_MappedDriveAndExtendedPathsUseTheSameShareKey) {
    const auto share = nasRoot();
    const auto drives = GetLogicalDrives();
    ASSERT_NE(drives, 0u);
    std::wstring device;
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if (!(drives & (1u << (letter - L'A')))) { device = std::wstring(1, letter) + L":"; break; }
    }
    ASSERT_FALSE(device.empty());
    std::wstring remote = share.wstring();
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpLocalName = device.data();
    resource.lpRemoteName = remote.data();
    ASSERT_EQ(WNetAddConnection2W(&resource, nullptr, nullptr, CONNECT_TEMPORARY), NO_ERROR);
    auto cleanup = wil::scope_exit([&] { WNetCancelConnection2W(device.c_str(), 0, FALSE); });
    const auto mapped = device + L"\\";
    const auto extended = L"\\\\?\\" + mapped;
    const auto expected = library::storageKey(share);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(library::storageKey(mapped), expected);
    EXPECT_EQ(library::storageKey(extended), expected);
    EXPECT_EQ(library::storageKey(device + L"\\missing-test-file.flac"), expected);
    ASSERT_EQ(WNetCancelConnection2W(device.c_str(), 0, FALSE), NO_ERROR);
    cleanup.release();
    wchar_t result[32768]{};
    DWORD size = DWORD(std::size(result));
    EXPECT_EQ(WNetGetConnectionW(device.c_str(), result, &size), ERROR_NOT_CONNECTED);
}

TEST(LibraryNas, DISABLED_ServerPlaybackOnNasDefersFingerprintsUntilPauseOrStop) {
    const auto share = nasRoot();
    const auto fixture = share / (L".camelplayer-nas-test-playback-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()));
    ASSERT_EQ(fixture.parent_path(), share);
    ASSERT_TRUE(fs::create_directory(fixture));
    auto cleanup = wil::scope_exit([&] {
        std::error_code error;
        for (const auto* name : {"track.flac", "pause.lrc", "stop.lrc"}) fs::remove(fixture / name, error);
        fs::remove(fixture, error);
    });
    const auto local = localRun("playback-gate");
    std::cout << "NAS playback results: " << camel::toUtf8(local.wstring()) << std::endl;
    fs::copy_file(testutil::testFile("16-44.1k"), fixture / "track.flac");
    LibraryStore store(local / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(fixture);
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_TRUE(scanner.fingerprint(root).complete);
    class PassiveOutput final : public camel::audio::IOutput {
    public:
        bool supports(const camel::audio::DeviceFormat&) override { return true; }
        void open(const camel::audio::DeviceFormat&, camel::audio::IRenderSource*) override {}
        void start() override {}
        void pause() override {}
        void close() override {}
        camel::audio::OutputStatus status() const override { return {"NAS test", 256, "No hardware"}; }
    };
    {
        LocalServer server(camel::Settings{}, local / "db" / "settings.json", [](const camel::Settings&) {
            return std::make_unique<PassiveOutput>();
        });
        for (const auto release : {CommandKind::Pause, CommandKind::Stop}) {
            Command play;
            play.kind = CommandKind::OpenAndPlay;
            play.source.emplace(camel::toUtf8((fixture / "track.flac").wstring()));
            server.submit(play);
            server.waitIdle();
            ASSERT_EQ(server.snapshot().playback.state, PlaybackState::Playing);
            const bool pause = release == CommandKind::Pause;
            std::ofstream(fixture / (pause ? "pause.lrc" : "stop.lrc")) << (pause ? "lyrics" : "different lyrics");
            ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NULL") == 1; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NULL"), 1);
            Command command;
            command.kind = release;
            server.submit(command);
            server.waitIdle();
            ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NULL") == 0; }));
        }
    }
    for (const auto* name : {"track.flac", "pause.lrc", "stop.lrc"}) ASSERT_TRUE(fs::remove(fixture / name));
    ASSERT_TRUE(fs::remove(fixture));
    EXPECT_FALSE(fs::exists(fixture));
    std::ofstream(local / "report.json") << Json{{"passed", !::testing::Test::HasFailure()},
                                                 {"pause_releases_fingerprints", true}, {"stop_releases_fingerprints", true},
                                                 {"nas_fixture_removed", true}}.dump(2);
}

TEST(LibraryNas, DISABLED_PhysicalCrossVolumeMovesPreserveOnlyVerifiedHistory) {
    const auto share = nasRoot();
    const auto remote = share / (L".camelplayer-nas-test-cross-volume-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64()));
    ASSERT_EQ(remote.parent_path(), share);
    ASSERT_TRUE(fs::create_directory(remote));
    const auto local = localRun("cross-volume");
    std::cout << "Cross-volume results: " << camel::toUtf8(local.wstring()) << std::endl;
    Json results = Json::array();
    for (const bool toNas : {true, false}) {
        for (const std::string scenario : {"verified", "no-fingerprint", "duplicate", "live-copy", "offline-source"}) {
            SCOPED_TRACE((toNas ? "local-to-NAS/" : "NAS-to-local/") + scenario);
            const auto name = (toNas ? "to-nas-" : "to-local-") + scenario;
            const auto caseDir = local / name;
            const auto localMusic = caseDir / "music", nasMusic = remote / name;
            const auto offline = (toNas ? localMusic : nasMusic).wstring() + L"-offline";
            fs::create_directories(localMusic);
            ASSERT_TRUE(fs::create_directory(nasMusic));
            auto cleanup = wil::scope_exit([&] {
                std::error_code error;
                for (const fs::path& folder : {nasMusic, fs::path(offline)}) {
                    if (folder.parent_path() != remote) continue;
                    fs::remove(folder / "track.flac", error);
                    fs::remove(folder / "duplicate.flac", error);
                    fs::remove(folder, error);
                }
            });
            const auto sourceFolder = toNas ? localMusic : nasMusic;
            const auto targetFolder = toNas ? nasMusic : localMusic;
            const auto source = sourceFolder / "track.flac", target = targetFolder / "track.flac";
            fs::copy_file(testutil::testFile("16-44.1k"), source);
            ASSERT_NE(library::storageKey(sourceFolder), library::storageKey(targetFolder));
            ASSERT_FALSE(library::storageKey(sourceFolder).empty());
            ASSERT_FALSE(library::storageKey(targetFolder).empty());
            LibraryStore store(caseDir / "db");
            LibraryScanner scanner(store);
            const auto sourceRoot = scanner.addRoot(sourceFolder), targetRoot = scanner.addRoot(targetFolder);
            ASSERT_TRUE(scanner.scan(sourceRoot).complete);
            const auto original = count(store, "SELECT id FROM files WHERE available=1");
            const auto track = count(store, "SELECT id FROM tracks");
            store.write([=](sqlite::Database& db) {
                db.prepare("INSERT INTO stats(file_id,rating,favorite,play_count,last_played) VALUES(?,5,1,17,123)")
                    .bind(1, original).run();
                db.exec("INSERT INTO playlists(id,name,created,modified) VALUES(1,'Kept',0,0)");
                db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,source_description) VALUES(1,0,?,'Kept')")
                    .bind(1, original).run();
                db.prepare("INSERT INTO queue(position,entry_id,file_id,path,source_description) VALUES(0,77,?,?,'Kept')")
                    .bind(1, original).bind(2, camel::toUtf8(source.wstring())).run();
            }).get();
            if (scenario == "duplicate") {
                fs::copy_file(source, sourceFolder / "duplicate.flac");
                ASSERT_TRUE(scanner.scan(sourceRoot).complete);
            }
            if (scenario != "no-fingerprint") ASSERT_TRUE(scanner.fingerprint(sourceRoot).complete);
            const auto digest = library::fileDigest(source, library::fileVersion(source), {}, {});
            ASSERT_TRUE(digest);
            fs::copy_file(source, target);
            ASSERT_EQ(library::fileDigest(target, library::fileVersion(target), {}, {}), digest);
            if (scenario == "offline-source") fs::rename(sourceFolder, fs::path(offline));
            else if (scenario != "live-copy") ASSERT_TRUE(fs::remove(source));
            const auto report = scanner.scan(targetRoot);
            ASSERT_TRUE(report.complete);
            const bool verified = scenario == "verified";
            EXPECT_EQ(report.moved, verified ? 1u : 0u);
            auto db = store.openReader();
            auto arrival = db.prepare("SELECT id FROM files WHERE root_id=? AND available=1");
            ASSERT_TRUE(arrival.bind(1, targetRoot).step());
            if (verified) {
                EXPECT_EQ(arrival.integer(0), original);
                EXPECT_EQ(count(store, "SELECT id FROM tracks WHERE file_id IN (SELECT file_id FROM stats)"), track);
                EXPECT_EQ(count(store, "SELECT COUNT(*) FROM queue JOIN files ON files.id=queue.file_id WHERE queue.path=files.path"), 1);
            } else EXPECT_NE(arrival.integer(0), original);
            EXPECT_EQ(count(store, "SELECT file_id FROM playlist_items"), original);
            EXPECT_EQ(count(store, "SELECT file_id FROM queue WHERE entry_id=77"), original);
            EXPECT_EQ(count(store, "SELECT rating FROM stats"), 5);
            EXPECT_EQ(count(store, "SELECT favorite FROM stats"), 1);
            EXPECT_EQ(count(store, "SELECT play_count FROM stats"), 17);
            EXPECT_EQ(count(store, "SELECT last_played FROM stats"), 123);
            if (scenario == "offline-source") fs::rename(fs::path(offline), sourceFolder);
            results.push_back({{"direction", toNas ? "local-to-NAS" : "NAS-to-local"}, {"scenario", scenario},
                               {"moved", report.moved}, {"original_file_id", original}, {"arrival_file_id", arrival.integer(0)},
                               {"source_storage", camel::toUtf8(library::storageKey(sourceFolder))},
                               {"target_storage", camel::toUtf8(library::storageKey(targetFolder))}});
        }
    }
    ASSERT_TRUE(fs::remove(remote));
    EXPECT_FALSE(fs::exists(remote));
    std::ofstream(local / "report.json") << Json{{"cases", results}, {"passed", !::testing::Test::HasFailure()},
                                                 {"nas_fixture_removed", true}}.dump(2);
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
