// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Six zones at once on this computer (docs/Task-breakdown.md MZ-R06): CPU, memory, threads and reads while they play
// through capture outputs, half of them from a NAS, with a scan of a NAS folder running meanwhile. Nothing is written to
// the NAS. Opt in with --gtest_also_run_disabled_tests --gtest_filter=ZoneLoad.*, CAMEL_ZONE_LOAD_FILES set to the NAS
// files to play, separated by '|', and CAMEL_ZONE_LOAD_SCAN to the NAS folder to scan.
#include <gtest/gtest.h>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>

#include "Common/util/Text.h"
#include "Engine/SlowSource.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::wstring environment(const wchar_t* name) {
    wchar_t value[32768]{};
    const DWORD length = GetEnvironmentVariableW(name, value, DWORD(std::size(value)));
    return length && length < std::size(value) ? std::wstring(value, length) : std::wstring();
}

std::vector<std::filesystem::path> split(const std::wstring& list) {
    std::vector<std::filesystem::path> paths;
    for (size_t at = 0; at <= list.size();) {
        const size_t end = std::min(list.find(L'|', at), list.size());
        if (end > at) paths.emplace_back(list.substr(at, end - at));
        at = end + 1;
    }
    return paths;
}

std::string device(size_t number) {
    return "load-" + std::to_string(number);
}

double seconds(FILETIME time) {
    return double(uint64_t(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 1e7;
}

struct Sample {
    double cpu = 0;          // seconds of CPU time used by the process
    double workingSet = 0;   // MB
    double privateBytes = 0; // MB
    double readBytes = 0;    // read by the process
    size_t threads = 0;
};

Sample sample() {
    Sample s;
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    s.cpu = seconds(kernel) + seconds(user);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    s.workingSet = double(memory.WorkingSetSize) / 1e6;
    s.privateBytes = double(memory.PrivateUsage) / 1e6;
    IO_COUNTERS io{};
    GetProcessIoCounters(GetCurrentProcess(), &io);
    s.readBytes = double(io.ReadTransferCount);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    for (BOOL more = Thread32First(snapshot, &thread); more; more = Thread32Next(snapshot, &thread))
        if (thread.th32OwnerProcessID == GetCurrentProcessId()) ++s.threads;
    CloseHandle(snapshot);
    return s;
}

} // namespace

TEST(ZoneLoad, DISABLED_SixZonesWithAScanMeanwhile) {
    const auto shared = split(environment(L"CAMEL_ZONE_LOAD_FILES"));
    const std::filesystem::path scanned = environment(L"CAMEL_ZONE_LOAD_SCAN");
    ASSERT_EQ(shared.size(), 3U) << "set CAMEL_ZONE_LOAD_FILES to three NAS files";
    ASSERT_FALSE(scanned.empty()) << "set CAMEL_ZONE_LOAD_SCAN to a NAS folder";
    const auto dir = testutil::tempDir() / ("zone-load-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir / "data");
    // Local tracks, the third upsampled, as a zone with a DAC of its own would be.
    const std::vector<std::filesystem::path> local = {
        testutil::generatedFile({"zone-load-1", 44100, 16, 2, 120.0}),
        testutil::generatedFile({"zone-load-2", 96000, 24, 2, 120.0}),
        testutil::generatedFile({"zone-load-3", 44100, 24, 2, 120.0}),
    };
    const auto rate = measureReadRate(shared.front().wstring(), 0, uint64_t{8} << 20);
    ASSERT_TRUE(rate) << "cannot read " << toUtf8(shared.front().wstring());

    Settings settings;
    settings.autoplay = false;
    settings.wasapi.deviceId = device(1);
    for (size_t i = 1; i <= 6; ++i) {
        SavedOutput output;
        output.backend = Backend::Wasapi;
        output.id = output.wasapi.deviceId = device(i);
        output.name = "Load " + std::to_string(i);
        output.enabled = true;
        output.zone.autoplay = false;
        if (i == 3) output.wasapi.upsampling = Upsampling::IntegerMultiple;
        settings.outputs.push_back(output);
    }
    const Sample idle = sample();
    {
        LocalServer server(fromJson(toJson(settings)), dir / "data" / "settings.json", [](const Settings&) {
            CaptureOptions options;
            options.file = L"NUL";
            options.realtime = true;
            return createCaptureOutput(options);
        });
        server.waitIdle();
        const auto zone = [](size_t number) { return zoneId(Backend::Wasapi, device(number)); };
        for (size_t i = 1; i <= 6; ++i) {
            Command play;
            play.kind = CommandKind::QueueReplace;
            play.zone = zone(i);
            play.sources.emplace_back(toUtf8((i <= 3 ? local[i - 1] : shared[i - 4]).wstring()));
            server.submit(play);
        }
        server.waitIdle();
        const auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE([&] {
            while (std::chrono::steady_clock::now() - start < 120s) {
                const auto s = server.snapshot();
                if (std::ranges::all_of(s.zones,
                                        [](const ZoneSnapshot& z) { return z.playback.state == PlaybackState::Playing; }))
                    return true;
                std::this_thread::sleep_for(100ms);
            }
            return false;
        }()) << "not every zone played";
        const auto playingAfter = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        // A scan of the NAS meanwhile, which gives way to the zones playing from it.
        Command add;
        add.kind = CommandKind::AddLibraryRoot;
        add.path = toUtf8(scanned.wstring());
        server.submit(add);
        server.waitIdle();

        nlohmann::json samples = nlohmann::json::array();
        Sample last = sample();
        auto lastAt = std::chrono::steady_clock::now();
        double peakCpu = 0, peakWorkingSet = 0, peakPrivate = 0;
        size_t peakThreads = 0;
        const auto measuredFrom = lastAt;
        const Sample first = last;
        for (int second = 1; second <= 90; ++second) {
            std::this_thread::sleep_for(1s);
            const Sample now = sample();
            const auto at = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(at - lastAt).count();
            const double cpu = (now.cpu - last.cpu) / elapsed * 100.0;
            const auto progress = server.libraryScanProgress();
            samples.push_back({{"s", second},
                               {"cpu_percent_of_one_core", cpu},
                               {"working_set_mb", now.workingSet},
                               {"private_mb", now.privateBytes},
                               {"threads", now.threads},
                               {"read_mb_per_s", (now.readBytes - last.readBytes) / elapsed / 1e6},
                               {"scan_checked", progress.checked}});
            peakCpu = std::max(peakCpu, cpu);
            peakWorkingSet = std::max(peakWorkingSet, now.workingSet);
            peakPrivate = std::max(peakPrivate, now.privateBytes);
            peakThreads = std::max(peakThreads, now.threads);
            last = now;
            lastAt = at;
        }
        const double span = std::chrono::duration<double>(lastAt - measuredFrom).count();
        const auto s = server.snapshot();
        nlohmann::json zones = nlohmann::json::array();
        for (const auto& z : s.zones)
            zones.push_back({{"zone", z.id},
                             {"file", z.playback.pathUtf8},
                             {"state", int(z.playback.state)},
                             {"underruns", z.playback.underruns},
                             {"buffering", z.playback.bufferingSeconds.value_or(0)},
                             {"error", z.playback.error ? errorCodeName(*z.playback.error) : ""},
                             {"plan_resample", z.playback.plan && z.playback.plan->resample}});
        const auto progress = server.libraryScanProgress();
        const nlohmann::json report{
            {"nas_read_rate_mb_per_s_before", *rate / 1e6},
            {"cores", std::thread::hardware_concurrency()},
            {"every_zone_playing_after_s", playingAfter},
            {"idle", {{"working_set_mb", idle.workingSet}, {"private_mb", idle.privateBytes}, {"threads", idle.threads}}},
            {"average_cpu_percent_of_one_core", (last.cpu - first.cpu) / span * 100.0},
            {"peak_cpu_percent_of_one_core", peakCpu},
            {"peak_working_set_mb", peakWorkingSet},
            {"peak_private_mb", peakPrivate},
            {"peak_threads", peakThreads},
            {"average_read_mb_per_s", (last.readBytes - first.readBytes) / span / 1e6},
            {"scan", {{"scans", progress.scans}, {"found", progress.found}, {"checked", progress.checked}}},
            {"zones", zones},
            {"samples", samples}};
        std::ofstream(dir / "report.json") << report.dump(2);
        std::cout << "zone load report: " << toUtf8((dir / "report.json").wstring()) << '\n'
                  << report.dump() << '\n'
                  << std::flush;
        for (const auto& z : s.zones) {
            EXPECT_EQ(z.playback.state, PlaybackState::Playing) << z.id;
            EXPECT_EQ(z.playback.underruns, 0U) << z.id;
        }
        server.shutdown();
    }
}
