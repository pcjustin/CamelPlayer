// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// One device, one zone on the DAVE and the Cirrus (docs/Task-breakdown.md MZ-R04): the DAVE's backends are one device,
// and its ASIO driver plays beside FlexASIO on the Cirrus. Quiet tones, -50 dBFS.
// Opt in with --gtest_also_run_disabled_tests --gtest_filter=ZoneHardware.*; MZ-R12's runs below also reach the network
// players, the bedroom's HomePods, HQPlayer and the NAS test folder.
#include <gtest/gtest.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "OutputBridge/IOutput.h"
#include "DsdFixture.h"
#include "Server/Playback/LocalServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::optional<DeviceEntry> named(const std::vector<DeviceEntry>& devices, const char* part) {
    const auto found = std::ranges::find_if(devices, [&](const DeviceEntry& d) {
        return d.name.find(part) != std::string::npos && d.name.find("unavailable") == std::string::npos;
    });
    return found == devices.end() ? std::nullopt : std::optional(*found);
}

SavedOutput enabled(Backend backend, const std::string& id) {
    SavedOutput output;
    output.backend = backend;
    output.id = output.wasapi.deviceId = output.asio.driverClsid = output.ks.deviceId = id;
    output.enabled = true;
    output.zone.autoplay = false;
    return output;
}

const ZoneSnapshot& in(const Snapshot& s, const std::string& id) {
    const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
    if (found == s.zones.end()) throw std::runtime_error("no zone " + id);
    return *found;
}

Command playIn(const std::string& zone, const std::filesystem::path& file) {
    Command c;
    c.kind = CommandKind::QueueReplace;
    c.zone = zone;
    c.sources.emplace_back(toUtf8(file.wstring()));
    return c;
}

bool waitFor(LocalServer& server, const std::function<bool(const Snapshot&)>& check, std::chrono::seconds time = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check(server.snapshot())) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

double seconds(const ZoneSnapshot& z) {
    return z.playback.source ? double(z.playback.position) / z.playback.source->sampleRate : 0.0;
}

} // namespace

TEST(ZoneHardware, DISABLED_TheDavesBackendsAreOneDeviceAndItsAsioDriverPlaysBesideFlexAsio) {
    const auto aDave = named(listAsioDrivers(), "Chord"), aFlex = named(listAsioDrivers(), "FlexASIO");
    const auto wDave = named(listWasapiDevices(), "Chord"), kDave = named(listKsPins(), "Chord");
    const auto cirrus = named(listWasapiDevices(), "Cirrus");
    if (!aDave || !wDave || !kDave || !cirrus) GTEST_SKIP() << "the DAVE or the Cirrus is not connected";
    if (!aFlex) GTEST_SKIP() << "FlexASIO is not installed";
    Settings settings;
    settings.autoplay = false;
    settings.wasapi.deviceId = cirrus->id;
    settings.outputs = {enabled(Backend::Asio, aDave->id), enabled(Backend::Wasapi, wDave->id), enabled(Backend::Ks, kDave->id),
                        enabled(Backend::Asio, aFlex->id), enabled(Backend::Wasapi, cirrus->id)};
    settings = fromJson(toJson(settings));
    const auto toneA = testutil::generatedFile({"zone-hardware-440", 96000, 24, 2, 30.0, true, -50.0});
    const auto toneB = testutil::generatedFile({"zone-hardware-48k", 48000, 24, 2, 30.0, true, -50.0});
    LocalServer server(settings);
    Command refresh;
    refresh.kind = CommandKind::RefreshDevices;
    server.submit(refresh);
    server.waitIdle();
    const auto asioDave = zoneId(Backend::Asio, aDave->id), wasapiDave = zoneId(Backend::Wasapi, wDave->id),
               ksDave = zoneId(Backend::Ks, kDave->id), flex = zoneId(Backend::Asio, aFlex->id);

    server.submit(playIn(asioDave, toneA));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return seconds(in(s, asioDave)) > 1.0; }));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return in(s, wasapiDave).inUseBy == asioDave; }));
    EXPECT_EQ(in(server.snapshot(), ksDave).inUseBy, asioDave);
    for (const auto& other : {wasapiDave, ksDave}) {
        server.submit(playIn(other, toneB));
        server.waitIdle();
        const auto s = server.snapshot();
        ASSERT_TRUE(s.commandError) << other;
        EXPECT_EQ(s.commandError->code, ErrorCode::DeviceInUse) << other;
        EXPECT_NE(s.commandError->detail.find("in use by"), std::string::npos) << s.commandError->detail;
    }

    // Two ASIO drivers at once, each with callbacks of its own.
    server.submit(playIn(flex, toneB));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return seconds(in(s, flex)) > 1.0; }));
    const auto before = server.snapshot();
    std::this_thread::sleep_for(10s);
    const auto after = server.snapshot();
    for (const auto& zone : {asioDave, flex}) {
        EXPECT_EQ(in(after, zone).playback.state, PlaybackState::Playing) << zone;
        EXPECT_FALSE(in(after, zone).playback.error) << zone << ": " << in(after, zone).playback.errorDetail;
        EXPECT_NEAR(seconds(in(after, zone)) - seconds(in(before, zone)), 10.0, 1.0) << zone;
        EXPECT_EQ(in(after, zone).playback.underruns, 0U) << zone;
    }
    for (const auto& zone : {asioDave, flex}) {
        Command stop;
        stop.kind = CommandKind::Stop;
        stop.zone = zone;
        server.submit(stop);
    }
    server.waitIdle();
}

// Every output this computer finds within half a minute, for MZ-R12's choice of devices.
TEST(ZoneHardware, DISABLED_ListEveryOutputFound) {
    Settings settings;
    settings.autoplay = false;
    LocalServer server(fromJson(toJson(settings)));
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        Command refresh;
        refresh.kind = CommandKind::RefreshDevices;
        server.submit(refresh);
        server.waitIdle();
        std::this_thread::sleep_for(2s);
    }
    const auto devices = server.snapshot().devices;
    const auto list = [](const char* backend, const std::vector<DeviceInfo>& found) {
        for (const auto& d : found)
            std::cout << backend << " | " << d.id << " | " << d.name << " | " << d.model << " | " << d.mac
                      << (d.speakers.empty() ? "" : " | pair") << (d.unsupported.empty() ? "" : " | " + d.unsupported) << '\n';
    };
    list("wasapi", devices.wasapi);
    list("asio", devices.asio);
    list("ks", devices.ks);
    list("network", devices.network);
    list("airplay", devices.airplay);
    list("hqplayer", devices.hqplayer);
    std::cout << std::flush;
    server.shutdown();
}

namespace {

// MZ-R12 (docs/Task-breakdown.md): the owner's devices, found by name, and quiet tones only (the owner, 2026-11-23).
struct Mz12Devices {
    std::string daveAsio, daveWasapi, flex, cirrus, dmpA6, keces, poly, bedroom, hqplayer;
};

std::string idNamed(const std::vector<DeviceInfo>& devices, const std::string& part) {
    const auto found = std::ranges::find_if(devices, [&](const DeviceInfo& d) { return d.name.find(part) != std::string::npos; });
    return found == devices.end() ? std::string() : found->id;
}

std::optional<Mz12Devices> findMz12Devices(LocalServer& server) {
    camel::log::init(testutil::tempDir() / "mz-r12" / "logs", L"mz-r12"); // what the players did, for a run that fails
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    Mz12Devices d;
    while (std::chrono::steady_clock::now() < deadline) {
        Command refresh;
        refresh.kind = CommandKind::RefreshDevices;
        server.submit(refresh);
        server.waitIdle();
        const auto devices = server.snapshot().devices;
        d = {idNamed(devices.asio, "Chord"),
             idNamed(devices.wasapi, "Chord"),
             idNamed(devices.asio, "FlexASIO"),
             idNamed(devices.wasapi, "Cirrus"),
             idNamed(devices.network, "DMP-A6"),
             idNamed(devices.network, "Essential Bravo"),
             idNamed(devices.network, "Poly"),
             idNamed(devices.airplay, "\xE4\xB8\xBB\xE8\x87\xA5"),
             devices.hqplayer.empty() ? std::string() : devices.hqplayer.front().id};
        if (!d.daveAsio.empty() && !d.cirrus.empty() && !d.dmpA6.empty() && !d.keces.empty() && !d.poly.empty() &&
            !d.bedroom.empty())
            return d;
        std::this_thread::sleep_for(2s);
    }
    std::cout << "not found: " << (d.daveAsio.empty() ? "DAVE " : "") << (d.cirrus.empty() ? "Cirrus " : "")
              << (d.dmpA6.empty() ? "DMP-A6 " : "") << (d.keces.empty() ? "KECES " : "") << (d.poly.empty() ? "Poly " : "")
              << (d.bedroom.empty() ? "bedroom " : "") << '\n';
    return std::nullopt;
}

SavedOutput mz12Output(Backend backend, const std::string& id) {
    SavedOutput output = enabled(backend, id);
    output.network.udn = output.airplay.deviceId = output.hqplayer.address = id;
    output.volumeMode = isNetworkPlayer(backend) ? VolumeMode::Player : VolumeMode::Fixed;
    return output;
}

// The owner's NAS test folder: copies only (docs, nas-test-folder).
std::filesystem::path mz12Nas() {
    const std::filesystem::path folder = L"\\\\TACHIKOMA\\Music\\_camelplayer-test\\mz-r12";
    std::filesystem::create_directories(folder);
    return folder;
}

std::filesystem::path onNas(const std::filesystem::path& local) {
    const auto copy = mz12Nas() / local.filename();
    std::filesystem::copy_file(local, copy, std::filesystem::copy_options::overwrite_existing);
    return copy;
}

// A quiet DSD sine, -50 dB of full modulation, written as DSF.
std::filesystem::path quietDsd(const std::filesystem::path& folder, uint32_t rate, double frequency, double seconds) {
    const auto file = folder / ("mz-r12-dsd" + std::to_string(rate / 44100 / 64 * 64) + "-" + std::to_string(int(frequency)) +
                                "-" + std::to_string(int(seconds)) + ".dsf");
    if (std::filesystem::exists(file)) return file;
    const auto raw = dsdfixture::dsdSine(rate, frequency, 0.0032, size_t(double(rate) / 8 * seconds));
    dsdfixture::DsfSpec spec;
    spec.rate = rate;
    dsdfixture::writeFile(file, dsdfixture::dsf(raw, spec));
    return file;
}

std::filesystem::path quietTone(const std::string& name, uint32_t rate, uint32_t bits, double seconds) {
    return testutil::generatedFile({name, rate, bits, 2, seconds, true, -50.0});
}

double cpuSeconds() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto value = [](FILETIME t) { return double(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7; };
    return value(kernel) + value(user);
}

double privateMb() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return double(memory.PrivateUsage) / 1e6;
}

Command zoneCommand(CommandKind kind, const std::string& zone) {
    Command c;
    c.kind = kind;
    c.zone = zone;
    return c;
}

Command queueIn(const std::string& zone, const std::vector<std::filesystem::path>& files) {
    Command c = zoneCommand(CommandKind::QueueReplace, zone);
    for (const auto& file : files) c.sources.emplace_back(toUtf8(file.wstring()));
    return c;
}

bool playingOn(const ZoneSnapshot& z) {
    return z.playback.state == PlaybackState::Playing && !z.playback.error;
}

std::string stateText(const ZoneSnapshot& z) {
    std::ostringstream text;
    text << (z.playback.state == PlaybackState::Playing  ? "playing"
             : z.playback.state == PlaybackState::Paused ? "paused"
                                                         : "stopped")
         << " at " << std::fixed << std::setprecision(1) << seconds(z) << " s, " << z.playback.underruns << " underruns";
    if (z.playback.bufferingSeconds) text << ", buffering";
    if (z.playback.error) text << ", " << errorCodeName(*z.playback.error) << ": " << z.playback.errorDetail;
    return text.str();
}

} // namespace

// Six zones, the DAVE, the Cirrus, the DMP-A6, the KECES, the Poly and the bedroom's HomePods, playing quiet tones of
// their own at once for half an hour (CAMEL_MZ12_MINUTES), the three network players' from the NAS and a DSD tone on the
// DAVE; each zone is paused, sought and moved on in turn while the others play on.
TEST(ZoneHardware, DISABLED_SixZonesPlayAtOnceForHalfAnHour) {
    wchar_t minutesText[16] = {};
    const double minutes =
        GetEnvironmentVariableW(L"CAMEL_MZ12_MINUTES", minutesText, 16) ? std::wcstod(minutesText, nullptr) : 30.0;
    Settings settings;
    settings.autoplay = false;
    LocalServer scout(fromJson(toJson(settings)));
    const auto d = findMz12Devices(scout);
    scout.shutdown();
    if (!d) GTEST_SKIP() << "not every device of MZ-R12 is on";
    settings.wasapi.deviceId = d->cirrus;
    settings.outputs = {mz12Output(Backend::Asio, d->daveAsio), mz12Output(Backend::Wasapi, d->cirrus),
                        mz12Output(Backend::Network, d->dmpA6), mz12Output(Backend::Network, d->keces),
                        mz12Output(Backend::Network, d->poly),  mz12Output(Backend::AirPlay, d->bedroom)};
    settings = fromJson(toJson(settings));
    const auto local = testutil::tempDir() / "mz-r12";
    std::filesystem::create_directories(local);
    struct Planned {
        std::string zone, name;
        std::vector<std::filesystem::path> files;
    };
    std::vector<Planned> zones{
        {zoneId(Backend::Asio, d->daveAsio),
         "DAVE (ASIO)",
         {quietDsd(local, 2822400, 440, 120), quietTone("mz-r12-dave", 44100, 24, 180)}},
        {zoneId(Backend::Wasapi, d->cirrus),
         "Cirrus (WASAPI)",
         {quietTone("mz-r12-cirrus-a", 48000, 24, 180), quietTone("mz-r12-cirrus-b", 48000, 16, 120)}},
        {zoneId(Backend::Network, d->dmpA6),
         "DMP-A6 (UPnP, NAS)",
         {onNas(quietTone("mz-r12-dmp-a", 96000, 24, 180)), onNas(quietTone("mz-r12-dmp-b", 44100, 16, 120))}},
        {zoneId(Backend::Network, d->keces),
         "KECES (UPnP, NAS)",
         {onNas(quietTone("mz-r12-keces-a", 44100, 16, 180)), onNas(quietTone("mz-r12-keces-b", 88200, 24, 120))}},
        {zoneId(Backend::Network, d->poly),
         "Poly (UPnP, NAS)",
         {onNas(quietTone("mz-r12-poly-a", 192000, 24, 180)), onNas(quietTone("mz-r12-poly-b", 48000, 16, 120))}},
        {zoneId(Backend::AirPlay, d->bedroom),
         "Bedroom (AirPlay)",
         {quietTone("mz-r12-bedroom-a", 44100, 16, 180), quietTone("mz-r12-bedroom-b", 44100, 16, 120)}},
    };
    LocalServer server(settings);
    ASSERT_TRUE(findMz12Devices(server));
    const double cpuBefore = cpuSeconds();
    const auto started = std::chrono::steady_clock::now();
    for (const auto& z : zones) {
        server.submit(queueIn(z.zone, z.files));
        Command repeat = zoneCommand(CommandKind::SetRepeat, z.zone);
        repeat.repeat = RepeatMode::All;
        server.submit(repeat);
    }
    server.waitIdle();
    ASSERT_TRUE(waitFor(
        server,
        [&](const Snapshot& s) { return std::ranges::all_of(zones, [&](const Planned& z) { return playingOn(in(s, z.zone)); }); },
        90s))
        << [&] {
               std::string why;
               for (const auto& z : zones) why += z.name + ": " + stateText(in(server.snapshot(), z.zone)) + "\n";
               return why;
           }();
    std::cout << "every zone playing after " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
              << " s\n";

    // Each zone's turn: paused 4 s, played, sought to 60 s, moved on; the others must move on meanwhile.
    std::map<std::string, int> stalls, errors;
    std::map<std::string, std::string> lastError;
    double peakMb = 0, peakCpu = 0;
    auto last = server.snapshot();
    double lastCpu = cpuSeconds();
    auto lastAt = std::chrono::steady_clock::now();
    const auto turnEvery = std::chrono::duration<double>(minutes * 60.0 / double(zones.size() + 1));
    size_t turn = 0;
    auto nextTurn = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(turnEvery);
    std::optional<std::string> busy; // the zone whose turn it is, which may stand still meanwhile
    std::chrono::steady_clock::time_point busyUntil;
    const auto end =
        started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(minutes * 60.0));
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(5s);
        const auto now = std::chrono::steady_clock::now();
        if (busy && now > busyUntil) busy.reset();
        if (turn < zones.size() && now >= nextTurn) {
            const auto& z = zones[turn];
            busy = z.zone;
            busyUntil = now + 20s;
            server.submit(zoneCommand(CommandKind::Pause, z.zone));
            server.waitIdle();
            const bool paused = in(server.snapshot(), z.zone).playback.state == PlaybackState::Paused;
            std::this_thread::sleep_for(4s);
            server.submit(zoneCommand(CommandKind::Play, z.zone));
            server.waitIdle();
            std::this_thread::sleep_for(3s);
            Command seek = zoneCommand(CommandKind::Seek, z.zone);
            const auto playing = in(server.snapshot(), z.zone);
            seek.frame = playing.playback.source ? uint64_t(60.0 * playing.playback.source->sampleRate) : 0;
            server.submit(seek);
            server.waitIdle();
            std::this_thread::sleep_for(3s);
            const uint64_t entry = in(server.snapshot(), z.zone).queueEntryId;
            server.submit(zoneCommand(CommandKind::Next, z.zone));
            server.waitIdle();
            std::this_thread::sleep_for(3s);
            const auto after = in(server.snapshot(), z.zone);
            std::cout << z.name << ": paused " << (paused ? "yes" : "no") << ", moved on "
                      << (after.queueEntryId != entry ? "yes" : "no") << ", " << stateText(after) << '\n';
            EXPECT_TRUE(paused) << z.name;
            EXPECT_NE(after.queueEntryId, entry) << z.name;
            ++turn;
            nextTurn += std::chrono::duration_cast<std::chrono::steady_clock::duration>(turnEvery);
        }
        const auto s = server.snapshot();
        const double elapsed = std::chrono::duration<double>(now - lastAt).count();
        const double cpu = cpuSeconds();
        peakCpu = std::max(peakCpu, (cpu - lastCpu) / elapsed * 100.0);
        peakMb = std::max(peakMb, privateMb());
        lastCpu = cpu;
        lastAt = now;
        for (const auto& z : zones) {
            const auto& was = in(last, z.zone);
            const auto& is = in(s, z.zone);
            if (is.playback.error) {
                ++errors[z.name];
                lastError[z.name] = stateText(is);
            }
            if (busy && *busy == z.zone) continue;
            const bool movedOn = is.queueEntryId != was.queueEntryId || seconds(is) > seconds(was) + 0.5;
            if (!playingOn(is) || !movedOn) {
                ++stalls[z.name];
                std::cout << "stood still: " << z.name << ", " << stateText(is) << '\n' << std::flush;
            }
        }
        last = s;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto s = server.snapshot();
    std::cout << "MZ-R12 six zones, " << wall / 60.0 << " minutes: CPU average " << (cpuSeconds() - cpuBefore) / wall * 100.0
              << "% of one processor, peak " << peakCpu << "%, peak private " << peakMb << " MB\n";
    for (const auto& z : zones) {
        const auto& is = in(s, z.zone);
        std::cout << "  " << z.name << ": " << stateText(is) << ", stood still " << stalls[z.name] << " times, errors seen "
                  << errors[z.name] << (lastError[z.name].empty() ? "" : " (" + lastError[z.name] + ")") << '\n';
        EXPECT_TRUE(playingOn(is)) << z.name;
        EXPECT_EQ(stalls[z.name], 0) << z.name;
        EXPECT_EQ(errors[z.name], 0) << z.name;
        if (z.zone.starts_with("asio:") || z.zone.starts_with("wasapi:")) EXPECT_EQ(is.playback.underruns, 0U) << z.name;
    }
    std::cout << std::flush;
    for (const auto& z : zones) server.submit(zoneCommand(CommandKind::Stop, z.zone));
    server.waitIdle();
    server.shutdown();
}

// HQPlayer on this computer, set to the DAVE, against the DAVE's own zone, in both orders: the second to start is
// refused, by the device or by HQPlayer, and the first plays on.
TEST(ZoneHardware, DISABLED_HQPlayerAndTheDavesOwnZoneInBothOrders) {
    Settings settings;
    settings.autoplay = false;
    LocalServer scout(fromJson(toJson(settings)));
    const auto d = findMz12Devices(scout);
    scout.shutdown();
    if (!d || d->hqplayer.empty()) GTEST_SKIP() << "the DAVE or HQPlayer is not found";
    settings.wasapi.deviceId = d->cirrus;
    settings.outputs = {mz12Output(Backend::Asio, d->daveAsio), mz12Output(Backend::HQPlayer, d->hqplayer),
                        mz12Output(Backend::Wasapi, d->cirrus)};
    settings = fromJson(toJson(settings));
    LocalServer server(settings);
    ASSERT_TRUE(findMz12Devices(server));
    const auto dave = zoneId(Backend::Asio, d->daveAsio), hqp = zoneId(Backend::HQPlayer, d->hqplayer);
    const auto toneA = quietTone("mz-r12-hqp-a", 96000, 24, 60), toneB = quietTone("mz-r12-hqp-b", 44100, 24, 60);
    for (const auto& [first, second] : {std::pair{hqp, dave}, std::pair{dave, hqp}}) {
        server.submit(queueIn(first, {toneA}));
        server.waitIdle();
        ASSERT_TRUE(waitFor(
            server, [&](const Snapshot& s) { return seconds(in(s, first)) > 3.0; }, 60s))
            << first << ": " << stateText(in(server.snapshot(), first));
        server.submit(queueIn(second, {toneB}));
        server.waitIdle();
        std::this_thread::sleep_for(12s);
        const auto s = server.snapshot();
        std::cout << first << " first: " << stateText(in(s, first)) << "\n  then " << second << ": " << stateText(in(s, second))
                  << (s.commandError ? "; command error " + s.commandError->detail : "") << '\n'
                  << std::flush;
        EXPECT_TRUE(playingOn(in(s, first))) << first;
        EXPECT_FALSE(playingOn(in(s, second)) && seconds(in(s, second)) > 5.0) << second << " played beside " << first;
        for (const auto& zone : {first, second}) server.submit(zoneCommand(CommandKind::Stop, zone));
        server.waitIdle();
        std::this_thread::sleep_for(5s); // HQPlayer lets the device go
    }
    server.shutdown();
}

// Three zones playing DSD256 from the NAS, started one after another: none breaks up as the next starts.
TEST(ZoneHardware, DISABLED_ThreeDsd256ZonesFromTheNasStartedOneAfterAnother) {
    Settings settings;
    settings.autoplay = false;
    LocalServer scout(fromJson(toJson(settings)));
    const auto d = findMz12Devices(scout);
    scout.shutdown();
    if (!d) GTEST_SKIP() << "not every device of MZ-R12 is on";
    settings.wasapi.deviceId = d->cirrus;
    settings.outputs = {mz12Output(Backend::Asio, d->daveAsio), mz12Output(Backend::Network, d->dmpA6),
                        mz12Output(Backend::Network, d->keces), mz12Output(Backend::Wasapi, d->cirrus)};
    settings.convertDsdWhenUnsupported = true; // the network players declare no DSD and take it as PCM
    settings = fromJson(toJson(settings));
    const auto nas = mz12Nas();
    const std::vector<std::string> order{zoneId(Backend::Asio, d->daveAsio), zoneId(Backend::Network, d->dmpA6),
                                         zoneId(Backend::Network, d->keces)};
    const std::vector<std::filesystem::path> files{quietDsd(nas, 11289600, 440, 90), quietDsd(nas, 11289600, 550, 90),
                                                   quietDsd(nas, 11289600, 660, 90)};
    LocalServer server(settings);
    ASSERT_TRUE(findMz12Devices(server));
    std::map<std::string, int> stalls;
    std::map<std::string, uint64_t> underruns;
    auto last = server.snapshot();
    for (size_t i = 0; i < order.size(); ++i) {
        server.submit(queueIn(order[i], {files[i]}));
        server.waitIdle();
        ASSERT_TRUE(waitFor(
            server, [&](const Snapshot& s) { return seconds(in(s, order[i])) > 1.0; }, 60s))
            << order[i] << ": " << stateText(in(server.snapshot(), order[i]));
        std::cout << order[i] << " started: " << stateText(in(server.snapshot(), order[i])) << '\n' << std::flush;
        // The ones already playing move on as it starts and after.
        last = server.snapshot();
        for (int second = 0; second < 20; ++second) {
            std::this_thread::sleep_for(1s);
            const auto s = server.snapshot();
            for (size_t j = 0; j < i; ++j) {
                const auto& is = in(s, order[j]);
                if (!playingOn(is) || seconds(is) <= seconds(in(last, order[j])) + 0.2 || is.playback.bufferingSeconds) {
                    ++stalls[order[j]];
                    std::cout << "broke up: " << order[j] << ", " << stateText(is) << '\n' << std::flush;
                }
            }
            last = s;
        }
    }
    const auto s = server.snapshot();
    for (const auto& zone : order) {
        std::cout << "  " << zone << ": " << stateText(in(s, zone)) << ", broke up " << stalls[zone] << " times\n";
        EXPECT_EQ(stalls[zone], 0) << zone;
        EXPECT_TRUE(playingOn(in(s, zone))) << zone;
    }
    EXPECT_EQ(in(s, order[0]).playback.underruns, 0U);
    std::cout << std::flush;
    for (const auto& zone : order) server.submit(zoneCommand(CommandKind::Stop, zone));
    server.waitIdle();
    server.shutdown();
}

// Two zones modulating PCM to DSD256 at once, the DAVE natively and the DMP-A6 as DoP, with CPU recorded; if this
// computer cannot keep up, the one started last pauses with MZ-R06's notice.
TEST(ZoneHardware, DISABLED_TwoZonesModulatePcmToDsd256AtOnce) {
    Settings settings;
    settings.autoplay = false;
    LocalServer scout(fromJson(toJson(settings)));
    const auto d = findMz12Devices(scout);
    scout.shutdown();
    if (!d) GTEST_SKIP() << "not every device of MZ-R12 is on";
    settings.wasapi.deviceId = d->cirrus;
    SavedOutput dave = mz12Output(Backend::Asio, d->daveAsio), dmp = mz12Output(Backend::Network, d->dmpA6);
    dave.asio.dsdUpsampling = {.mode = DsdUpsampling::Set, .rate = 256, .pcm = true};
    dmp.network.dsdUpsampling = {.mode = DsdUpsampling::Set, .rate = 256, .pcm = true};
    settings.outputs = {dave, dmp, mz12Output(Backend::Wasapi, d->cirrus)};
    settings = fromJson(toJson(settings));
    LocalServer server(settings);
    ASSERT_TRUE(findMz12Devices(server));
    const auto daveZone = zoneId(Backend::Asio, d->daveAsio), dmpZone = zoneId(Backend::Network, d->dmpA6);
    const double cpuBefore = cpuSeconds();
    const auto started = std::chrono::steady_clock::now();
    server.submit(queueIn(daveZone, {quietTone("mz-r12-mod-a", 96000, 24, 90)}));
    server.waitIdle();
    ASSERT_TRUE(waitFor(
        server, [&](const Snapshot& s) { return seconds(in(s, daveZone)) > 2.0; }, 60s))
        << stateText(in(server.snapshot(), daveZone));
    server.submit(queueIn(dmpZone, {quietTone("mz-r12-mod-b", 44100, 24, 90)}));
    server.waitIdle();
    double peak = 0, lastCpu = cpuSeconds();
    auto lastAt = std::chrono::steady_clock::now();
    for (int second = 0; second < 60; ++second) {
        std::this_thread::sleep_for(1s);
        const auto now = std::chrono::steady_clock::now();
        const double cpu = cpuSeconds();
        peak = std::max(peak, (cpu - lastCpu) / std::chrono::duration<double>(now - lastAt).count() * 100.0);
        lastCpu = cpu;
        lastAt = now;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto s = server.snapshot();
    const auto& a = in(s, daveZone);
    const auto& b = in(s, dmpZone);
    std::cout << "two zones modulating to DSD256: CPU average " << (cpuSeconds() - cpuBefore) / wall * 100.0
              << "% of one processor, peak " << peak << "%\n  DAVE: " << stateText(a)
              << (a.playback.plan ? ", modulator order " + std::to_string(a.playback.plan->modulatorOrder) : "")
              << "\n  DMP-A6: " << stateText(b)
              << (b.playback.plan ? ", modulator order " + std::to_string(b.playback.plan->modulatorOrder) : "") << '\n'
              << std::flush;
    // Either both keep up, or the one started last gave way with the notice.
    const bool gaveWay = b.playback.error == ErrorCode::CannotKeepUp && playingOn(a);
    EXPECT_TRUE((playingOn(a) && playingOn(b) && a.playback.underruns == 0) || gaveWay);
    for (const auto& zone : {daveZone, dmpZone}) server.submit(zoneCommand(CommandKind::Stop, zone));
    server.waitIdle();
    server.shutdown();
}

// One network player alone (CAMEL_MZ12_PLAYER, part of its name), playing the six-zone run's NAS tones, its state every
// two seconds: for a fault MZ-R12 found.
TEST(ZoneHardware, DISABLED_OneNetworkPlayerFromTheNas) {
    wchar_t part[64] = {};
    GetEnvironmentVariableW(L"CAMEL_MZ12_PLAYER", part, 64);
    const std::string name = toUtf8(part);
    if (name.empty()) GTEST_SKIP() << "set CAMEL_MZ12_PLAYER";
    Settings settings;
    settings.autoplay = false;
    LocalServer scout(fromJson(toJson(settings)));
    const auto d = findMz12Devices(scout);
    scout.shutdown();
    if (!d) GTEST_SKIP();
    LocalServer probe(fromJson(toJson(settings)));
    findMz12Devices(probe);
    const std::string udn = idNamed(probe.snapshot().devices.network, name);
    probe.shutdown();
    ASSERT_FALSE(udn.empty()) << name;
    settings.wasapi.deviceId = d->cirrus;
    settings.outputs = {mz12Output(Backend::Network, udn), mz12Output(Backend::Wasapi, d->cirrus)};
    settings = fromJson(toJson(settings));
    LocalServer server(settings);
    ASSERT_TRUE(findMz12Devices(server));
    const auto zone = zoneId(Backend::Network, udn);
    server.submit(
        queueIn(zone, {onNas(quietTone("mz-r12-keces-a", 44100, 16, 180)), onNas(quietTone("mz-r12-dmp-a", 96000, 24, 180))}));
    server.waitIdle();
    for (int second = 0; second < 120; second += 2) {
        std::this_thread::sleep_for(2s);
        std::cout << second + 2 << " s: " << stateText(in(server.snapshot(), zone)) << '\n' << std::flush;
    }
    server.submit(zoneCommand(CommandKind::Stop, zone));
    server.waitIdle();
    server.shutdown();
}
