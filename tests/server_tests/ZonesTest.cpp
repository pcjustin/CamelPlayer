// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Zones on one Server (docs/Plan.md §5.12): every enabled output plays a queue of its own, and what one zone does leaves
// the others as they were.
#include <gtest/gtest.h>

#include <windows.h>
#include <powerbase.h>
#pragma comment(lib, "powrprof.lib")

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "Common/util/Text.h"
#include "Engine/SamplePacker.h"
#include "Engine/SlowSource.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Api/ApiJson.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "Server/Playback/QueueSession.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::string device(size_t number) {
    return "zone-" + std::to_string(number);
}

std::string zone(size_t number) {
    return zoneId(Backend::Wasapi, device(number));
}

// `count` WASAPI outputs enabled, the first in use.
Settings zoneSettings(size_t count) {
    Settings s;
    s.autoplay = false;
    s.wasapi.deviceId = device(1);
    for (size_t i = 1; i <= count; ++i) {
        SavedOutput output;
        output.backend = Backend::Wasapi;
        output.id = output.wasapi.deviceId = device(i);
        output.name = "Zone " + std::to_string(i);
        output.enabled = true;
        output.zone.autoplay = false;
        s.outputs.push_back(output);
    }
    return fromJson(toJson(s)); // as a settings file holds them
}

class ZonesTest;

// A capture device that counts its opens and closes and can fail as an unplugged device does.
class ZoneOutput final : public IOutput {
public:
    ZoneOutput(ZonesTest& test, std::string device, std::unique_ptr<IOutput> inner);
    ~ZoneOutput() override;
    bool supports(const DeviceFormat& f) override { return inner_->supports(f); }
    void open(const DeviceFormat& f, IRenderSource* source) override;
    void start() override { inner_->start(); }
    void pause() override { inner_->pause(); }
    void close() override;
    OutputStatus status() const override { return inner_->status(); }
    void fail() { raiseError(Error(ErrorCode::DeviceLost, "the test unplugged the device")); }

private:
    ZonesTest& test_;
    std::string device_;
    std::unique_ptr<IOutput> inner_;
    bool open_ = false;
};

class ZonesTest : public ::testing::Test {
public:
    std::mutex mutex;
    std::map<std::string, int> opens, closes;
    std::map<std::string, ZoneOutput*> live;

protected:
    std::filesystem::path dir;
    std::unique_ptr<LocalServer> server;

    // With `saved`, the settings and the library live in the test's folder and survive restart().
    void start(size_t count, const std::function<void(Settings&)>& change = {}, bool saved = false) {
        dir = testutil::tempDir() / "zones" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "data");
        settingsFile = saved ? dir / "data" / "settings.json" : std::filesystem::path{};
        Settings settings = zoneSettings(count);
        if (change) change(settings);
        open(fromJson(toJson(settings)));
    }

    void open(const Settings& settings) {
        server = std::make_unique<LocalServer>(settings, settingsFile, [this](const Settings& s) {
            const std::string name = s.backend == Backend::Ks     ? s.ks.deviceId
                                     : s.backend == Backend::Asio ? s.asio.driverClsid
                                     : s.wasapi.deviceId.empty()  ? std::string("default")
                                                                  : s.wasapi.deviceId;
            CaptureOptions options;
            options.file = capture(name);
            options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
            options.realtime = true;
            return std::make_unique<ZoneOutput>(*this, name, createCaptureOutput(options));
        });
        if (localDevices)
            server->setLocalDevices([this] {
                std::scoped_lock lock(mutex);
                return *localDevices;
            });
        server->waitIdle();
    }

    // A new Server on what the last one saved, which the change turns into what an earlier version wrote.
    void restart(const std::function<void(nlohmann::json&)>& change = {}) {
        server.reset();
        if (change) {
            auto j = nlohmann::json::parse(std::ifstream(settingsFile));
            change(j);
            std::ofstream(settingsFile, std::ios::trunc) << j.dump(2);
        }
        open(loadSettings(settingsFile));
    }

    std::filesystem::path settingsFile;
    std::optional<LocalDevices> localDevices; // under mutex; what device scans find, set before start()

    void TearDown() override {
        server.reset();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    std::filesystem::path capture(const std::string& name) const { return dir / (name + ".raw"); }

    static Command command(CommandKind kind, size_t number) {
        Command c;
        c.kind = kind;
        c.zone = zone(number);
        return c;
    }

    static Command play(size_t number, const std::vector<std::filesystem::path>& files) {
        Command c = command(CommandKind::QueueReplace, number);
        for (const auto& file : files) c.sources.emplace_back(toUtf8(file.wstring()));
        return c;
    }

    Snapshot send(Command c) {
        server->submit(std::move(c));
        server->waitIdle();
        return server->snapshot();
    }

    static const ZoneSnapshot& of(const Snapshot& s, size_t number) {
        const auto found = std::ranges::find(s.zones, zone(number), &ZoneSnapshot::id);
        if (found == s.zones.end()) throw std::runtime_error("no zone " + zone(number));
        return *found;
    }

    bool waitFor(const std::function<bool(const Snapshot&)>& check, std::chrono::milliseconds time = 20s) {
        const auto deadline = std::chrono::steady_clock::now() + time * testutil::kSlowdown;
        while (std::chrono::steady_clock::now() < deadline) {
            if (check(server->snapshot())) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    static bool playing(const ZoneSnapshot& z, double seconds = 0.2) {
        return z.playback.state == PlaybackState::Playing && z.playback.source &&
               z.playback.position > uint64_t(seconds * z.playback.source->sampleRate);
    }

    static bool ended(const ZoneSnapshot& z) {
        return !z.queue.entries.empty() && z.queueEntryId == z.queue.entries.back().id &&
               z.playback.state == PlaybackState::Stopped;
    }

    // What the zone's device was handed, as 24-bit samples.
    std::vector<int32_t> captured(size_t number) const {
        std::ifstream in(capture(device(number)), std::ios::binary | std::ios::ate);
        if (!in) return {};
        std::vector<uint8_t> bytes(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        std::vector<int32_t> samples(bytes.size() / 4);
        unpackSamples(bytes.data(), samples.size(), 24, SampleType::Int32, samples.data());
        return samples;
    }

    static std::vector<int32_t> reference(const std::vector<std::filesystem::path>& files) {
        std::vector<int32_t> samples;
        for (const auto& file : files) {
            const auto decoded = testaudio::referenceDecode(file);
            EXPECT_TRUE(decoded.error.empty()) << decoded.error;
            samples.insert(samples.end(), decoded.samples.begin(), decoded.samples.end());
        }
        return samples;
    }

    void fail(size_t number) {
        std::scoped_lock lock(mutex);
        live.at(device(number))->fail();
    }

    int opened(size_t number) {
        std::scoped_lock lock(mutex);
        return opens[device(number)];
    }

    int closed(size_t number) {
        std::scoped_lock lock(mutex);
        return closes[device(number)];
    }
};

ZoneOutput::ZoneOutput(ZonesTest& test, std::string device, std::unique_ptr<IOutput> inner)
    : test_(test), device_(std::move(device)), inner_(std::move(inner)) {}

ZoneOutput::~ZoneOutput() {
    std::scoped_lock lock(test_.mutex);
    if (test_.live[device_] == this) test_.live.erase(device_);
}

void ZoneOutput::open(const DeviceFormat& f, IRenderSource* source) {
    inner_->open(f, source);
    std::scoped_lock lock(test_.mutex);
    ++test_.opens[device_];
    test_.live[device_] = this;
    open_ = true;
}

void ZoneOutput::close() {
    inner_->close();
    std::scoped_lock lock(test_.mutex);
    if (std::exchange(open_, false)) ++test_.closes[device_];
}

// Formats of their own, so that no two zones' tracks hold the same samples.
std::filesystem::path track(size_t number, double seconds) {
    static constexpr uint32_t kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    const uint32_t rate = kRates[(number - 1) % std::size(kRates)];
    return testutil::generatedFile(
        {"zone-track-" + std::to_string(number) + "-" + std::to_string(int(seconds * 10)), rate, 24, 2, seconds});
}

} // namespace

TEST_F(ZonesTest, EveryEnabledOutputIsAZoneInTheOutputMenusOrder) {
    start(3);
    const auto s = server->snapshot();
    ASSERT_EQ(s.zones.size(), 3U);
    for (size_t i = 1; i <= 3; ++i) {
        EXPECT_EQ(s.zones[i - 1].id, zone(i));
        EXPECT_EQ(s.zones[i - 1].backend, Backend::Wasapi);
        EXPECT_EQ(s.zones[i - 1].output, device(i));
        EXPECT_EQ(s.zones[i - 1].name, "Zone " + std::to_string(i));
    }
    EXPECT_EQ(s.selectedZone, zone(1));
}

TEST_F(ZonesTest, TwoZonesPlayDifferentTracksAtOnce) {
    start(2);
    const std::vector one{track(1, 1.5)}, two{track(2, 1.5)};
    send(play(1, one));
    send(play(2, two));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 2)); }));
    const auto both = server->snapshot();
    EXPECT_EQ(of(both, 1).playback.pathUtf8, toUtf8(one[0].wstring()));
    EXPECT_EQ(of(both, 2).playback.pathUtf8, toUtf8(two[0].wstring()));
    // The window follows the selected zone, the first.
    EXPECT_EQ(both.queueEntryId, of(both, 1).queueEntryId);
    EXPECT_EQ(both.playback.pathUtf8, of(both, 1).playback.pathUtf8);
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }));
    EXPECT_EQ(captured(1), reference(one));
    EXPECT_EQ(captured(2), reference(two));
}

TEST_F(ZonesTest, SixZonesPlayDifferentTracksAtOnce) {
    start(6);
    std::vector<std::vector<std::filesystem::path>> tracks;
    for (size_t i = 1; i <= 6; ++i) {
        tracks.push_back({track(i, 1.5)});
        send(play(i, tracks.back()));
    }
    EXPECT_TRUE(waitFor(
        [](const Snapshot& s) { return std::ranges::all_of(s.zones, [](const ZoneSnapshot& z) { return playing(z); }); }));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return std::ranges::all_of(s.zones, ended); }));
    const auto s = server->snapshot();
    for (size_t i = 1; i <= 6; ++i) {
        EXPECT_FALSE(of(s, i).playback.error) << of(s, i).playback.errorDetail;
        EXPECT_EQ(captured(i), reference(tracks[i - 1])) << zone(i);
    }
}

TEST_F(ZonesTest, EachZonePlaysItsQueueGaplessly) {
    start(2);
    const std::vector one{track(1, 0.8), track(1, 0.3), track(1, 0.6)};
    const std::vector two{track(4, 0.5), track(4, 0.7)};
    send(play(1, one));
    send(play(2, two));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }));
    EXPECT_EQ(opened(1), 1);
    EXPECT_EQ(opened(2), 1);
    EXPECT_EQ(captured(1), reference(one));
    EXPECT_EQ(captured(2), reference(two));
}

TEST_F(ZonesTest, EachZoneCrossfadesItsOwnTracks) {
    start(2, [](Settings& s) {
        s.crossfade = {true, 1.0};
        for (auto& output : s.outputs) output.zone.crossfade = {true, 1.0};
    });
    const std::vector one{track(2, 2.0), track(2, 2.5)};
    const std::vector two{track(5, 2.0), track(5, 2.5)};
    send(play(1, one));
    send(play(2, two));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }, 30s));
    // Each zone's second track came in a second before its first ended.
    for (size_t i = 1; i <= 2; ++i) {
        const double seconds = double(captured(i).size()) / 2 / (i == 1 ? 48000 : 176400);
        EXPECT_NEAR(seconds, 3.5, 0.25) << zone(i);
    }
}

TEST_F(ZonesTest, ASeekNextStopAndErrorInOneZoneLeaveTheOthersAsTheyWere) {
    start(4);
    for (size_t i = 1; i <= 4; ++i) send(play(i, {track(i, 7.0), track(i, 6.0)}));
    ASSERT_TRUE(waitFor(
        [](const Snapshot& s) { return std::ranges::all_of(s.zones, [](const ZoneSnapshot& z) { return playing(z, 0.5); }); }));
    Snapshot before = server->snapshot();
    // Played on from where it was: no jump back and none further than the time that passed.
    const auto unchanged = [&](size_t number) {
        const auto now = server->snapshot();
        const auto& was = of(before, number);
        const auto& is = of(now, number);
        EXPECT_EQ(is.queueEntryId, was.queueEntryId) << zone(number);
        EXPECT_EQ(is.queue.entries, was.queue.entries) << zone(number);
        EXPECT_EQ(is.playback.state, PlaybackState::Playing) << zone(number);
        EXPECT_FALSE(is.playback.error) << zone(number) << ": " << is.playback.errorDetail;
        const uint64_t rate = was.playback.source->sampleRate;
        EXPECT_GE(is.playback.position + rate / 10, was.playback.position) << zone(number);
        EXPECT_LT(is.playback.position, was.playback.position + 3 * rate) << zone(number);
    };

    Command seek = command(CommandKind::Seek, 1);
    constexpr uint64_t kFourSeconds = uint64_t{4} * 44100;
    seek.frame = kFourSeconds;
    send(std::move(seek));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return of(s, 1).playback.position >= kFourSeconds; }));
    for (const size_t other : {2, 3, 4}) unchanged(other);

    before = server->snapshot();
    send(command(CommandKind::Next, 2));
    const uint64_t second = of(before, 2).queue.entries[1].id;
    ASSERT_TRUE(waitFor([second](const Snapshot& s) { return of(s, 2).queueEntryId == second && playing(of(s, 2)); }));
    for (const size_t other : {1, 3, 4}) unchanged(other);

    before = server->snapshot();
    EXPECT_EQ(send(command(CommandKind::Stop, 3)).zones[2].playback.state, PlaybackState::Stopped);
    for (const size_t other : {1, 2, 4}) unchanged(other);

    before = server->snapshot();
    Command missing = command(CommandKind::OpenAndPlay, 4);
    missing.source.emplace(toUtf8((dir / "missing.flac").wstring()));
    send(std::move(missing));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return of(s, 4).playback.error == ErrorCode::FileOpen; }));
    for (const size_t other : {1, 2}) unchanged(other);
    EXPECT_EQ(of(server->snapshot(), 3).playback.state, PlaybackState::Stopped);
}

TEST_F(ZonesTest, ADeviceFaultInOneZoneLeavesTheOthersPlaying) {
    start(3);
    for (size_t i = 1; i <= 3; ++i) send(play(i, {track(i, 6.0)}));
    ASSERT_TRUE(waitFor(
        [](const Snapshot& s) { return std::ranges::all_of(s.zones, [](const ZoneSnapshot& z) { return playing(z, 0.3); }); }));
    fail(2);
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return of(s, 2).playback.state == PlaybackState::Paused; }));
    const auto s = server->snapshot();
    EXPECT_EQ(of(s, 2).playback.error, ErrorCode::DeviceLost);
    for (const size_t other : {1, 3}) {
        EXPECT_EQ(of(s, other).playback.state, PlaybackState::Playing) << zone(other);
        EXPECT_FALSE(of(s, other).playback.error) << zone(other);
    }
    const uint64_t one = of(s, 1).playback.position;
    EXPECT_TRUE(waitFor([one](const Snapshot& now) { return of(now, 1).playback.position > one + 44100 / 2; }));
}

TEST_F(ZonesTest, AZoneThatIsNotPlayingHoldsNoDevice) {
    start(3);
    send(play(2, {track(2, 4.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 2)); }));
    EXPECT_EQ(opened(1), 0);
    EXPECT_EQ(opened(3), 0);
    send(command(CommandKind::Stop, 2));
    EXPECT_EQ(opened(2), 1);
    EXPECT_EQ(closed(2), 1);
}

TEST_F(ZonesTest, ACommandForAZoneThatIsNotThereChangesNothing) {
    start(2);
    send(play(1, {track(1, 4.0)}));
    Command next = command(CommandKind::Next, 1);
    next.zone = zoneId(Backend::Asio, "not-enabled");
    const auto s = send(std::move(next));
    ASSERT_TRUE(s.commandError);
    EXPECT_NE(s.commandError->detail.find("no such zone"), std::string::npos) << s.commandError->detail;
    EXPECT_EQ(of(s, 1).queue.entries.size(), 1U);
    EXPECT_NE(of(s, 1).playback.state, PlaybackState::Stopped);
}

TEST_F(ZonesTest, OnlyPlaybackQueueAndOutputCommandsNameAZone) {
    start(2);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = server->snapshot().settings;
    apply.zone = zone(2);
    EXPECT_THROW(server->submit(apply), std::invalid_argument);
    apply.zone.clear();
    EXPECT_NO_THROW(server->submit(apply));
}

TEST_F(ZonesTest, EachZonesStateIsCurrentAfterCommandsToAnother) {
    start(2);
    send(play(1, {track(1, 6.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)); }));
    const auto before = server->snapshot();
    send(play(2, {track(2, 0.5)}));
    for (int i = 0; i < 20; ++i) {
        Command add = command(CommandKind::QueueAdd, 2);
        add.sources.emplace_back(toUtf8(track(2, 0.5).wstring()));
        server->submit(std::move(add));
    }
    server->waitIdle();
    const auto after = server->snapshot();
    EXPECT_EQ(of(after, 2).queue.entries.size(), 21U);
    EXPECT_EQ(of(after, 1).queue, of(before, 1).queue);
    EXPECT_GT(after.revision, before.revision);
    // Zone 1 goes on being published while zone 2 takes commands.
    const uint64_t position = of(after, 1).playback.position;
    EXPECT_TRUE(waitFor([position](const Snapshot& s) { return of(s, 1).playback.position > position; }));
}

// Choosing another output in use selects its zone; every zone keeps its queue and plays on (MZ-R07).
TEST_F(ZonesTest, ChangingTheOutputInUseSelectsItsZoneAndMovesNoQueue) {
    start(2);
    send(play(1, {track(1, 6.0), track(1, 1.0)}));
    send(play(2, {track(2, 6.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1), 0.5) && playing(of(s, 2), 0.2); }));
    const auto before = server->snapshot();
    EXPECT_EQ(before.selectedZone, zone(1));
    Settings s = nativeSettings(before.settings);
    selectOutput(s, Backend::Wasapi, device(2), before.devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    send(std::move(apply));
    const auto after = server->snapshot();
    EXPECT_EQ(after.selectedZone, zone(2));
    EXPECT_EQ(of(after, 1).queue.entries, of(before, 1).queue.entries);
    EXPECT_EQ(of(after, 2).queue.entries, of(before, 2).queue.entries);
    EXPECT_EQ(of(after, 1).playback.state, PlaybackState::Playing);
    EXPECT_EQ(of(after, 2).playback.state, PlaybackState::Playing);
    EXPECT_EQ(after.queueEntryId, of(after, 2).queueEntryId);
    EXPECT_EQ(after.queue.entries, of(after, 2).queue.entries);
    // A command naming no zone acts on the one selected now.
    Command pause;
    pause.kind = CommandKind::Pause;
    send(pause);
    EXPECT_EQ(of(server->snapshot(), 2).playback.state, PlaybackState::Paused);
    EXPECT_EQ(of(server->snapshot(), 1).playback.state, PlaybackState::Playing);
}

TEST_F(ZonesTest, PauseAllPausesEveryZonePlaying) {
    start(3);
    send(play(1, {track(1, 6.0)}));
    send(play(3, {track(3, 6.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 3)); }));
    Command all;
    all.kind = CommandKind::PauseAll;
    const auto s = send(all);
    EXPECT_FALSE(s.commandError);
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Paused);
    EXPECT_EQ(of(s, 2).playback.state, PlaybackState::Stopped);
    EXPECT_EQ(of(s, 3).playback.state, PlaybackState::Paused);
    EXPECT_EQ(commandFromJson(toJson(all)), all);
}

TEST(ZonesApiJsonTest, ZonesAndTheZoneACommandNamesRoundTrip) {
    Snapshot s;
    s.revision = 7;
    for (size_t i = 1; i <= 2; ++i) {
        ZoneSnapshot z;
        z.id = zone(i);
        z.output = device(i);
        z.name = "Zone " + std::to_string(i);
        z.queueEntryId = i;
        z.track.emplace("C:/music/" + std::to_string(i) + ".flac");
        z.playback.state = i == 1 ? PlaybackState::Playing : PlaybackState::Paused;
        z.playback.position = 1000 * i;
        s.zones.push_back(z);
    }
    s.selectedZone = zone(2);
    s.queueEntryId = s.zones[1].queueEntryId;
    s.track = s.zones[1].track;
    s.queue = s.zones[1].queue;
    s.playback = s.zones[1].playback;
    const auto j = toJson(s);
    EXPECT_FALSE(j.contains("queue")); // the selected zone's state is not repeated
    EXPECT_EQ(snapshotFromJson(j), s);

    Command c;
    c.kind = CommandKind::Pause;
    c.zone = zone(2);
    EXPECT_EQ(commandFromJson(toJson(c)), c);
}

namespace {

const SavedOutput& savedOutput(const Settings& s, size_t number) {
    const auto found = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.id == device(number); });
    if (found == s.outputs.end()) throw std::runtime_error("no saved " + device(number));
    return *found;
}

Command shuffle(size_t number, bool on) {
    Command c;
    c.kind = CommandKind::SetShuffle;
    c.shuffle = on;
    c.zone = zone(number);
    return c;
}

Command repeat(size_t number, RepeatMode mode) {
    Command c;
    c.kind = CommandKind::SetRepeat;
    c.repeat = mode;
    c.zone = zone(number);
    return c;
}

} // namespace

TEST(ZoneSettingsTest, AnEarlierVersionsCrossfadeAndAutoplayBecomeEveryOutputsOwn) {
    Settings old = zoneSettings(2);
    old.crossfade = {true, 4.0};
    old.autoplay = false;
    auto j = nlohmann::json::parse(toJson(old));
    j["schema"] = 4;
    j.erase("shuffle");
    j.erase("repeat");
    for (auto& output : j["outputs"]) output.erase("zone");
    const Settings read = fromJson(j.dump());
    EXPECT_TRUE(read.adoptQueueChoices);
    for (size_t i = 1; i <= 2; ++i) {
        EXPECT_EQ(savedOutput(read, i).zone.crossfade, (CrossfadeSettings{true, 4.0})) << i;
        EXPECT_FALSE(savedOutput(read, i).zone.autoplay) << i;
    }
    // Saved again, each output has its own.
    Settings changed = read;
    changed.outputs[1].zone.crossfade = {true, 2.0};
    changed.adoptQueueChoices = false;
    const Settings again = fromJson(toJson(changed));
    EXPECT_FALSE(again.adoptQueueChoices);
    EXPECT_EQ(savedOutput(again, 1).zone.crossfade, (CrossfadeSettings{true, 4.0}));
    EXPECT_EQ(savedOutput(again, 2).zone.crossfade, (CrossfadeSettings{true, 2.0}));
}

TEST_F(ZonesTest, TheQueuesShuffleAndRepeatOfAnEarlierVersionBecomeEveryZones) {
    start(3, {}, true);
    send(play(1, {track(1, 6.0), track(1, 1.0)}));
    send(shuffle(1, true));
    send(repeat(1, RepeatMode::All));
    restart([&](nlohmann::json& j) {
        j["schema"] = 4;
        j.erase("shuffle");
        j.erase("repeat");
        for (auto& output : j["outputs"]) output.erase("zone");
        // As an earlier version left the library: one queue, and nothing of the other zones.
        LibraryStore store(dir / "data");
        store
            .write([](sqlite::Database& db) {
                db.exec("DELETE FROM queue WHERE zone<>'wasapi:zone-1';"
                        "DELETE FROM app_state WHERE key LIKE 'session%' AND key NOT LIKE '%:wasapi:zone-1';"
                        "UPDATE queue SET zone='';"
                        "UPDATE app_state SET key=replace(key,'wasapi:zone-1','') WHERE key LIKE 'session%';");
            })
            .get();
    });
    const auto s = server->snapshot();
    for (size_t i = 1; i <= 3; ++i) {
        EXPECT_TRUE(of(s, i).queue.shuffle) << zone(i);
        EXPECT_EQ(of(s, i).queue.repeat, RepeatMode::All) << zone(i);
    }
    EXPECT_EQ(of(s, 1).queue.entries.size(), 2U);
    server->shutdown();
    const Settings saved = loadSettings(settingsFile);
    EXPECT_FALSE(saved.adoptQueueChoices);
    EXPECT_TRUE(saved.shuffle);
    EXPECT_TRUE(savedOutput(saved, 3).zone.shuffle);
    EXPECT_EQ(savedOutput(saved, 3).zone.repeat, RepeatMode::All);
}

TEST_F(ZonesTest, EachZonesShuffleAndRepeatAreItsOwnAndSurviveARestart) {
    start(3, {}, true);
    send(shuffle(2, true));
    send(repeat(3, RepeatMode::One));
    const auto s = server->snapshot();
    EXPECT_FALSE(of(s, 1).queue.shuffle);
    EXPECT_EQ(of(s, 1).queue.repeat, RepeatMode::Off);
    EXPECT_TRUE(of(s, 2).queue.shuffle);
    EXPECT_EQ(of(s, 2).queue.repeat, RepeatMode::Off);
    EXPECT_FALSE(of(s, 3).queue.shuffle);
    EXPECT_EQ(of(s, 3).queue.repeat, RepeatMode::One);
    restart();
    const auto after = server->snapshot();
    EXPECT_FALSE(of(after, 1).queue.shuffle);
    EXPECT_TRUE(of(after, 2).queue.shuffle);
    EXPECT_EQ(of(after, 3).queue.repeat, RepeatMode::One);
    EXPECT_EQ(of(after, 1).queue.repeat, RepeatMode::Off);
}

TEST_F(ZonesTest, EachZoneCrossfadesByItsOwnSetting) {
    start(2, [](Settings& s) { s.outputs[1].zone.crossfade = {true, 1.0}; });
    const std::vector one{track(2, 2.0), track(2, 2.5)};
    const std::vector two{track(5, 2.0), track(5, 2.5)};
    send(play(1, one));
    send(play(2, two));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }, 30s));
    EXPECT_EQ(captured(1), reference(one));
    EXPECT_NEAR(double(captured(2).size()) / 2 / 176400, 3.5, 0.25);
}

TEST_F(ZonesTest, AChangeToTheSelectedZonesSettingsLeavesTheOthersAlone) {
    start(2, [](Settings& s) { s.outputs[1].zone.crossfade = {true, 6.0}; });
    Settings s = nativeSettings(server->snapshot().settings);
    s.crossfade = {true, 2.0};
    s.autoplay = true;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    send(std::move(apply));
    server->shutdown();
    const auto settings = nativeSettings(server->snapshot().settings);
    EXPECT_EQ(settings.crossfade, (CrossfadeSettings{true, 2.0}));
    EXPECT_EQ(savedOutput(settings, 2).zone.crossfade, (CrossfadeSettings{true, 6.0}));
    EXPECT_FALSE(savedOutput(settings, 2).zone.autoplay);
}

TEST_F(ZonesTest, AnOutputTurnedOffAndOnAgainKeepsItsZonesSettings) {
    start(3);
    send(shuffle(2, true));
    send(repeat(2, RepeatMode::All));
    const auto setEnabled = [&](bool enabled) {
        Settings s = nativeSettings(server->snapshot().settings);
        setOutputEnabled(s, Backend::Wasapi, device(2), enabled, server->snapshot().devices);
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(s);
        return send(std::move(apply));
    };
    const auto off = setEnabled(false);
    EXPECT_EQ(off.zones.size(), 2U);
    EXPECT_TRUE(std::ranges::none_of(off.zones, [](const ZoneSnapshot& z) { return z.id == zone(2); }));
    const auto on = setEnabled(true);
    ASSERT_EQ(on.zones.size(), 3U);
    EXPECT_TRUE(of(on, 2).queue.shuffle);
    EXPECT_EQ(of(on, 2).queue.repeat, RepeatMode::All);
    EXPECT_FALSE(of(on, 3).queue.shuffle);
}

namespace {

std::vector<std::string> paths(const ZoneSnapshot& z) {
    std::vector<std::string> result;
    for (const auto& entry : z.queue.entries) result.push_back(entry.source.pathUtf8());
    return result;
}

size_t current(const ZoneSnapshot& z) {
    const auto at = std::ranges::find(z.queue.entries, z.queueEntryId, &QueueEntry::id);
    return size_t(at - z.queue.entries.begin());
}

} // namespace

TEST_F(ZonesTest, EveryZonesQueueComesBackPausedAtTheNextStart) {
    start(3, {}, true);
    for (size_t i = 1; i <= 3; ++i) send(play(i, {track(i, 7.0), track(i, 1.0)}));
    send(command(CommandKind::Next, 3));
    ASSERT_TRUE(waitFor(
        [](const Snapshot& s) { return std::ranges::all_of(s.zones, [](const ZoneSnapshot& z) { return playing(z, 0.5); }); }));
    const auto before = server->snapshot();
    restart();
    const auto after = server->snapshot();
    for (size_t i = 1; i <= 3; ++i) {
        const auto& was = of(before, i);
        const auto& is = of(after, i);
        EXPECT_EQ(paths(is), paths(was)) << zone(i);
        EXPECT_EQ(current(is), current(was)) << zone(i);
        EXPECT_EQ(is.playback.state, PlaybackState::Paused) << zone(i);
        EXPECT_GE(is.playback.position + 4410, was.playback.position) << zone(i);
    }
}

TEST_F(ZonesTest, TheQueueSavedBeforeZonesComesBackInTheZoneOfTheOutputInUse) {
    start(3, [](Settings& s) { s.wasapi.deviceId = device(2); }, true);
    send(play(2, {track(2, 6.0), track(2, 1.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 2)); }));
    const auto before = server->snapshot();
    server.reset();
    {
        // As schema 26 leaves the one queue an earlier version saved.
        LibraryStore store(dir / "data");
        store
            .write([](sqlite::Database& db) {
                db.exec("UPDATE queue SET zone='';"
                        "UPDATE app_state SET key=replace(key,'wasapi:zone-2','') WHERE key LIKE 'session%';");
            })
            .get();
    }
    open(loadSettings(settingsFile));
    const auto after = server->snapshot();
    EXPECT_EQ(paths(of(after, 2)), paths(of(before, 2)));
    EXPECT_EQ(of(after, 2).playback.state, PlaybackState::Paused);
    EXPECT_TRUE(of(after, 1).queue.entries.empty());
    EXPECT_TRUE(of(after, 3).queue.entries.empty());
    server->shutdown();
    LibraryStore store(dir / "data");
    auto db = store.openReader();
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue WHERE zone=''"), 0);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue WHERE zone='wasapi:zone-2'"), 2);
}

TEST_F(ZonesTest, AnOutputTurnedOffWhilePlayingStopsAndKeepsItsQueueForWhenItIsOn) {
    start(3, {}, true);
    send(play(2, {track(2, 6.0), track(2, 1.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 2), 0.5); }));
    const auto before = server->snapshot();
    const auto setEnabled = [&](bool enabled) {
        Settings s = nativeSettings(server->snapshot().settings);
        setOutputEnabled(s, Backend::Wasapi, device(2), enabled, server->snapshot().devices);
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(s);
        return send(std::move(apply));
    };
    const auto off = setEnabled(false);
    EXPECT_TRUE(std::ranges::none_of(off.zones, [](const ZoneSnapshot& z) { return z.id == zone(2); }));
    EXPECT_EQ(opened(2), closed(2));
    const auto on = setEnabled(true);
    EXPECT_EQ(paths(of(on, 2)), paths(of(before, 2)));
    EXPECT_EQ(of(on, 2).playback.state, PlaybackState::Paused);
    EXPECT_GE(of(on, 2).playback.position + 4410, of(before, 2).playback.position);
}

TEST_F(ZonesTest, DeletingAnOutputDeletesItsQueueOnceSavedAsAPlaylistWhenAsked) {
    start(3, {}, true);
    send(play(1, {track(1, 6.0)}));
    send(play(2, {track(2, 6.0), track(2, 1.0)}));
    send(play(3, {track(3, 6.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 2)) && playing(of(s, 3)); }));
    const auto saved = server->saveQueueAsPlaylist("Zone 2's queue", zone(2));
    EXPECT_EQ(saved.added, 2);
    Command remove;
    remove.kind = CommandKind::DeleteOutput;
    remove.zone = zone(2);
    const auto s = send(std::move(remove));
    EXPECT_TRUE(std::ranges::none_of(s.zones, [](const ZoneSnapshot& z) { return z.id == zone(2); }));
    EXPECT_EQ(opened(2), closed(2));
    EXPECT_EQ(of(s, 3).playback.state, PlaybackState::Playing);
    // Deleted in Settings › Audio outputs, as the window does it through the settings.
    Settings settings = nativeSettings(s.settings);
    deleteOutput(settings, Backend::Wasapi, device(3), s.devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    send(std::move(apply));
    const auto playlists = server->playlists();
    server->shutdown();
    ASSERT_EQ(playlists.size(), 1U);
    EXPECT_EQ(playlists.front().tracks, 2);
    LibraryStore store(dir / "data");
    auto db = store.openReader();
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue WHERE zone='wasapi:zone-2'"), 0);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue WHERE zone='wasapi:zone-3'"), 0);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue WHERE zone='wasapi:zone-1'"), 1);
    EXPECT_FALSE(hasSavedSession(db, zone(2)));
}

TEST_F(ZonesTest, PlaysInTwoZonesAtOnceAreEachCountedOnceUnderTheirZonesNames) {
    start(2, {}, true);
    const Settings settings = nativeSettings(server->snapshot().settings);
    server.reset();
    std::filesystem::create_directories(dir / "music");
    const std::vector files{dir / "music" / "one.flac", dir / "music" / "two.flac"};
    std::filesystem::copy_file(track(1, 1.5), files[0]);
    std::filesystem::copy_file(track(2, 1.5), files[1]);
    {
        LibraryStore store(dir / "data");
        LibraryScanner scanner(store);
        ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    }
    open(settings);
    send(play(1, {files[0]}));
    send(play(2, {files[1]}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }));
    server->shutdown();
    LibraryStore store(dir / "data");
    auto db = store.openReader();
    EXPECT_EQ(db.scalar("SELECT count(*) FROM play_events"), 2);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM play_events WHERE counted=1"), 2);
    EXPECT_EQ(db.scalar("SELECT count(DISTINCT file_id) FROM play_events"), 2);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM play_events WHERE zone='Zone 1'"), 1);
    EXPECT_EQ(db.scalar("SELECT count(*) FROM play_events WHERE zone='Zone 2'"), 1);
    EXPECT_EQ(db.scalar("SELECT sum(play_count) FROM stats"), 2);
}

// Two zones playing at once count both plays' listening time in full, and the history names each play's zone (MZ-R11).
TEST_F(ZonesTest, PlaysAtOnceInTwoZonesAddUpTheirListeningTimeAndTheHistoryNamesTheirZones) {
    start(2, {}, true);
    const Settings settings = nativeSettings(server->snapshot().settings);
    server.reset();
    std::filesystem::create_directories(dir / "music");
    const std::vector files{dir / "music" / "one.flac", dir / "music" / "two.flac"};
    std::filesystem::copy_file(track(1, 2.0), files[0]);
    std::filesystem::copy_file(track(2, 2.0), files[1]);
    {
        LibraryStore store(dir / "data");
        LibraryScanner scanner(store);
        ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    }
    open(settings);
    send(play(1, {files[0]}));
    send(play(2, {files[1]}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return ended(of(s, 1)) && ended(of(s, 2)); }));
    server->waitIdle();
    const auto stats = server->playStatistics({});
    EXPECT_EQ(stats.overview.plays, 2);
    // Each two-second play counted in full though they overlapped.
    EXPECT_GE(stats.overview.listenedMs, 3600);
    EXPECT_LE(stats.overview.listenedMs, 4400);
    ASSERT_EQ(stats.history.size(), 2U);
    std::vector<std::string> zones{stats.history[0].zone, stats.history[1].zone};
    std::ranges::sort(zones);
    EXPECT_EQ(zones, (std::vector<std::string>{"Zone 1", "Zone 2"}));
}

namespace {

SavedOutput enabled(Backend backend, const std::string& id, const std::string& name) {
    SavedOutput output;
    output.backend = backend;
    output.id = output.wasapi.deviceId = output.asio.driverClsid = output.ks.deviceId = id;
    output.name = name;
    output.enabled = true;
    output.zone.autoplay = false;
    return output;
}

// The DAVE through WASAPI, Kernel Streaming and its ASIO driver, the Cirrus, and FlexASIO, as this computer lists them.
LocalDevices daveAndCirrus() {
    LocalDevices d;
    d.wasapi = {{"w-dave", "Digital Output (Chord Async USB 44.1kHz-768kHz)", "{C-DAVE}"},
                {"w-cirrus", "Speakers (Cirrus Logic CS4208 (AB 126))", "{C-CIRRUS}"}};
    d.ks = {{"k-dave", "Chord Async USB 44.1kHz-768kHz / Chord Electronics Ltd. Streaming [Standard]", "{C-DAVE}"}};
    d.asio = {{"a-dave", "ASIO Chord  1.05", ""}, {"a-flex", "FlexASIO", ""}};
    d.defaultWasapi = "w-cirrus";
    return d;
}

void outputsOf(Settings& s, std::vector<SavedOutput> outputs, Backend inUse, const std::string& id) {
    s.outputs = std::move(outputs);
    s.backend = inUse;
    s.wasapi.deviceId = inUse == Backend::Wasapi ? id : "";
    s.asio.driverClsid = inUse == Backend::Asio ? id : "";
    s.ks.deviceId = inUse == Backend::Ks ? id : "";
}

Command playIn(const std::string& zoneOf, const std::filesystem::path& file) {
    Command c;
    c.kind = CommandKind::QueueReplace;
    c.zone = zoneOf;
    c.sources.emplace_back(toUtf8(file.wstring()));
    return c;
}

Command refresh() {
    Command c;
    c.kind = CommandKind::RefreshDevices;
    return c;
}

const ZoneSnapshot& in(const Snapshot& s, const std::string& id) {
    const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
    if (found == s.zones.end()) throw std::runtime_error("no zone " + id);
    return *found;
}

} // namespace

TEST_F(ZonesTest, AHeldDeviceIsInUseForEveryOtherZoneOfItAndPlayThereIsRefused) {
    localDevices = daveAndCirrus();
    start(0, [](Settings& s) {
        outputsOf(s,
                  {enabled(Backend::Wasapi, "w-dave", "DAVE"), enabled(Backend::Ks, "k-dave", "DAVE KS"),
                   enabled(Backend::Asio, "a-dave", "DAVE ASIO"), enabled(Backend::Wasapi, "w-cirrus", "Cirrus")},
                  Backend::Wasapi, "w-cirrus");
    });
    send(refresh());
    const auto wDave = zoneId(Backend::Wasapi, "w-dave"), kDave = zoneId(Backend::Ks, "k-dave"),
               aDave = zoneId(Backend::Asio, "a-dave"), cirrus = zoneId(Backend::Wasapi, "w-cirrus");
    send(playIn(wDave, track(1, 6.0)));
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return playing(in(s, wDave)); }));
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return in(s, kDave).inUseBy == wDave; }));
    auto s = server->snapshot();
    EXPECT_EQ(in(s, aDave).inUseBy, wDave); // the driver that names Chord is the DAVE
    EXPECT_TRUE(in(s, cirrus).inUseBy.empty());
    EXPECT_TRUE(in(s, wDave).inUseBy.empty());
    for (const auto& other : {kDave, aDave}) {
        s = send(playIn(other, track(2, 3.0)));
        ASSERT_TRUE(s.commandError) << other;
        EXPECT_EQ(s.commandError->code, ErrorCode::DeviceInUse) << other;
        EXPECT_EQ(s.commandError->detail, "in use by " + in(s, wDave).name);
        EXPECT_NE(in(s, other).playback.state, PlaybackState::Playing) << other;
    }
    {
        std::scoped_lock lock(mutex);
        EXPECT_EQ(opens["k-dave"], 0);
        EXPECT_EQ(opens["a-dave"], 0); // the driver was never asked
    }
    send(playIn(cirrus, track(3, 3.0)));
    EXPECT_TRUE(waitFor([&](const Snapshot& now) { return playing(in(now, cirrus)) && playing(in(now, wDave)); }));
}

TEST_F(ZonesTest, TwoDacsOfOneNameAreTwoDevicesAndTwoAsioDriversPlayAtOnce) {
    localDevices = daveAndCirrus();
    localDevices->wasapi.push_back({"w-dac1", "USB DAC", "{C-1}"});
    localDevices->wasapi.push_back({"w-dac2", "USB DAC", "{C-2}"});
    start(0, [](Settings& s) {
        outputsOf(s,
                  {enabled(Backend::Wasapi, "w-dac1", "USB DAC"), enabled(Backend::Wasapi, "w-dac2", "USB DAC 2"),
                   enabled(Backend::Asio, "a-dave", "DAVE ASIO"), enabled(Backend::Asio, "a-flex", "FlexASIO")},
                  Backend::Wasapi, "w-dac1");
    });
    send(refresh());
    const std::vector zonesOf{zoneId(Backend::Wasapi, "w-dac1"), zoneId(Backend::Wasapi, "w-dac2"),
                              zoneId(Backend::Asio, "a-dave"), zoneId(Backend::Asio, "a-flex")};
    for (size_t i = 0; i < zonesOf.size(); ++i) {
        const auto s = send(playIn(zonesOf[i], track(i + 1, 4.0)));
        EXPECT_FALSE(s.commandError) << zonesOf[i];
    }
    EXPECT_TRUE(waitFor([](const Snapshot& s) {
        return std::ranges::all_of(s.zones, [](const ZoneSnapshot& z) { return playing(z, 0.3) && z.inUseBy.empty(); });
    }));
}

TEST_F(ZonesTest, ADevicePausedLongEnoughToBeReleasedGoesToTheNextZone) {
    localDevices = daveAndCirrus();
    start(0, [](Settings& s) {
        outputsOf(s, {enabled(Backend::Wasapi, "w-dave", "DAVE"), enabled(Backend::Ks, "k-dave", "DAVE KS")}, Backend::Wasapi,
                  "w-dave");
    });
    send(refresh());
    const auto wDave = zoneId(Backend::Wasapi, "w-dave"), kDave = zoneId(Backend::Ks, "k-dave");
    send(playIn(wDave, track(1, 8.0)));
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return playing(in(s, wDave)); }));
    Command pause;
    pause.kind = CommandKind::Pause;
    pause.zone = wDave;
    send(std::move(pause));
    EXPECT_EQ(in(server->snapshot(), kDave).inUseBy, wDave); // a paused zone holds its device for a while
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return in(s, kDave).inUseBy.empty(); }));
    const auto s = send(playIn(kDave, track(2, 3.0)));
    EXPECT_FALSE(s.commandError);
    EXPECT_TRUE(waitFor([&](const Snapshot& now) { return playing(in(now, kDave)); }));
    EXPECT_EQ(in(server->snapshot(), wDave).inUseBy, kDave);
}

TEST_F(ZonesTest, TheWindowsDefaultMovingOntoAHeldDevicePausesTheZoneFollowingIt) {
    localDevices = daveAndCirrus();
    start(0, [](Settings& s) {
        outputsOf(s, {enabled(Backend::Wasapi, "", "Default"), enabled(Backend::Wasapi, "w-dave", "DAVE")}, Backend::Wasapi,
                  "w-dave");
    });
    send(refresh());
    const auto followed = zoneId(Backend::Wasapi, ""), wDave = zoneId(Backend::Wasapi, "w-dave");
    send(playIn(wDave, track(1, 8.0)));
    send(playIn(followed, track(2, 8.0)));
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return playing(in(s, wDave)) && playing(in(s, followed)); }));
    {
        std::scoped_lock lock(mutex);
        localDevices->defaultWasapi = "w-dave";
    }
    send(refresh());
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return in(s, followed).playback.state == PlaybackState::Paused; }));
    EXPECT_EQ(in(server->snapshot(), followed).inUseBy, wDave);
    EXPECT_EQ(in(server->snapshot(), wDave).playback.state, PlaybackState::Playing);
}

TEST_F(ZonesTest, OtherZonesPlayOnWhileTheOutputInUseChanges) {
    start(3);
    send(play(1, {track(1, 6.0)}));
    send(play(3, {track(3, 6.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 3)); }));
    Settings s = nativeSettings(server->snapshot().settings);
    selectOutput(s, Backend::Wasapi, device(2), server->snapshot().devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    send(std::move(apply));
    EXPECT_EQ(server->snapshot().selectedZone, zone(2));
    EXPECT_TRUE(waitFor([](const Snapshot& now) { return playing(of(now, 1), 0.8) && playing(of(now, 3), 0.8); }));
    EXPECT_EQ(opened(1), 1);
    EXPECT_EQ(closed(1), 0);
    EXPECT_EQ(opened(3), 1);
    EXPECT_EQ(closed(3), 0);
}

// Background work on the storage a zone plays from gives way to it, on other storage it takes one worker while any zone
// plays, and it runs at full speed again once every zone is paused (docs/Plan.md §7.7).
TEST_F(ZonesTest, AScanYieldsToTheZoneOnItsShareWhileAnotherPlaysFromALocalDiskUntilEveryZoneIsPaused) {
    const auto local = track(1, 30.0), shared = track(2, 30.0);
    // The local disk through its administrative share stands for a NAS: another storage to the library.
    const std::filesystem::path share = L"\\\\localhost\\" + shared.root_name().wstring().substr(0, 1) + L"$";
    const auto remote = share / shared.relative_path();
    if (!std::filesystem::exists(remote)) GTEST_SKIP() << "no administrative share to stand for a NAS";
    const std::filesystem::path elsewhere = L"\\\\elsewhere\\music";
    start(2);
    send(play(1, {local}));
    send(play(2, {remote}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 2)); }));
    EXPECT_NE(server->backgroundPace(share), BackgroundPace::Full);
    EXPECT_NE(server->backgroundPace(local.parent_path()), BackgroundPace::Full);
    EXPECT_EQ(server->backgroundPace(elsewhere), BackgroundPace::One);

    // With only the local zone playing, the share takes one worker.
    send(command(CommandKind::Pause, 2));
    EXPECT_EQ(server->backgroundPace(share), BackgroundPace::One);
    EXPECT_NE(server->backgroundPace(local.parent_path()), BackgroundPace::Full);

    send(command(CommandKind::Pause, 1));
    EXPECT_EQ(server->backgroundPace(share), BackgroundPace::Full);
    EXPECT_EQ(server->backgroundPace(local.parent_path()), BackgroundPace::Full);
    EXPECT_EQ(server->backgroundPace(elsewhere), BackgroundPace::Full);
}

// A zone starting from a share another zone plays from reads only what that zone leaves of the share's rate: the first
// plays on untouched, and the second buffers on what is left, where alone it would play at once (docs/Plan.md §5.12).
TEST_F(ZonesTest, ASecondZoneOnASlowShareLeavesTheFirstItsReadsAndBuffersOnWhatIsLeft) {
    const auto first = testutil::generatedFile({"zone-share-first", 384000, 24, 2, 30.0});
    const auto second = testutil::generatedFile({"zone-share-second", 352800, 24, 2, 10.0});
    const double needed = double(std::filesystem::file_size(first)) / 30.0;
    ASSERT_GT(needed, 1e6) << "a share's rate is measured only for files played at 1 MB/s or more";
    // Read at 1.6 times what the first plays at: enough for either alone, not for both at once.
    setSlowShareForTest(first.parent_path().wstring(), needed * 1.6);
    struct Restore {
        ~Restore() { setSlowShareForTest({}, 0); }
    } restore;
    start(2);
    send(play(1, {first}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1), 1.0); }));
    send(play(2, {second}));
    std::optional<uint32_t> buffering;
    ASSERT_TRUE(waitFor([&](const Snapshot& s) {
        if (!buffering) buffering = of(s, 2).playback.bufferingSeconds;
        return playing(of(s, 2), 0.5);
    }));
    // Read at what the first leaves, 0.6 of its rate, against 0.92 of it the second plays at, the second waits about
    // nine seconds; reckoned on the share alone, it would have played at once.
    ASSERT_TRUE(buffering) << "the second zone did not buffer";
    EXPECT_GE(*buffering, 5U);
    EXPECT_LE(*buffering, 20U);
    const auto s = server->snapshot();
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Playing);
    EXPECT_FALSE(of(s, 1).playback.bufferingSeconds);
    EXPECT_EQ(of(s, 1).playback.underruns, 0U);
    EXPECT_FALSE(of(s, 1).playback.error);
}

// While the DSP stage cannot keep up with two zones upsampling, the one started last pauses with the notice and the other
// plays on; playing it again clears the notice (docs/Plan.md §5.12).
TEST_F(ZonesTest, AComputerThatCannotKeepUpPausesTheZoneStartedLastAmongThoseUpsampling) {
    start(2, [](Settings& s) {
        s.wasapi.upsampling = Upsampling::IntegerMultiple;
        for (auto& output : s.outputs) output.wasapi.upsampling = Upsampling::IntegerMultiple;
    });
    send(play(1, {track(1, 30.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1), 0.5); }));
    send(play(2, {track(2, 30.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 2), 0.5); }));
    for (const size_t number : {size_t{1}, size_t{2}}) {
        const auto plan = of(server->snapshot(), number).playback.plan;
        ASSERT_TRUE(plan && plan->resample && plan->format.sampleRate > 48000) << "zone " << number << " does not upsample";
    }
    setDspStageDelayForTest(400ms);
    const bool paused = waitFor([](const Snapshot& s) { return of(s, 2).playback.state == PlaybackState::Paused; });
    setDspStageDelayForTest(0ms);
    ASSERT_TRUE(paused);
    auto s = server->snapshot();
    EXPECT_EQ(of(s, 2).playback.error, ErrorCode::CannotKeepUp);
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Playing);
    const uint64_t at = of(s, 1).playback.position;
    EXPECT_TRUE(waitFor([&](const Snapshot& now) { return of(now, 1).playback.position > at + 44100; }));
    s = server->snapshot();
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Playing);
    EXPECT_FALSE(of(s, 1).playback.error);

    send(command(CommandKind::Play, 2));
    EXPECT_TRUE(waitFor(
        [](const Snapshot& now) { return of(now, 2).playback.state == PlaybackState::Playing && !of(now, 2).playback.error; }));
}

// Each output keeps its own software volume; one saved before zones takes the one level every output had then (MZ-R07).
TEST(ZoneSettingsTest, EachOutputKeepsItsOwnSoftwareVolumeAndAnEarlierLevelBecomesEveryOutputs) {
    Settings old = zoneSettings(2);
    old.volume = {VolumeMode::Software, -20.0, true};
    auto j = nlohmann::json::parse(toJson(old));
    for (auto& output : j["outputs"]) {
        output.erase("volumeDb");
        output.erase("volumeMuted");
    }
    const Settings read = fromJson(j.dump());
    for (size_t i = 1; i <= 2; ++i) {
        EXPECT_EQ(savedOutput(read, i).volumeDb, -20.0) << i;
        EXPECT_TRUE(savedOutput(read, i).volumeMuted) << i;
    }
    Settings changed = read;
    changed.outputs[1].volumeDb = -35.5;
    EXPECT_EQ(savedOutput(fromJson(toJson(changed)), 2).volumeDb, -35.5);
    EXPECT_EQ(savedOutput(fromJson(toJson(changed)), 1).volumeDb, -20.0);
}

TEST_F(ZonesTest, EachZonePlaysAtItsOwnSoftwareVolume) {
    start(2, [](Settings& s) {
        s.volume = {VolumeMode::Software, -6.0, false};
        for (auto& output : s.outputs) {
            output.volumeMode = VolumeMode::Software;
            output.volumeDb = output.id == device(2) ? -30.0 : -6.0;
        }
    });
    send(play(1, {track(1, 6.0)}));
    send(play(2, {track(2, 6.0)}));
    const auto at = [](const Snapshot& s, size_t number) {
        const auto& plan = of(s, number).playback.plan;
        return plan ? plan->volumeDb : 1.0;
    };
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 2)); }));
    EXPECT_EQ(at(server->snapshot(), 1), -6.0);
    EXPECT_EQ(at(server->snapshot(), 2), -30.0);

    // Another zone's level changes in its saved output, leaving the selected one's.
    Settings s = nativeSettings(server->snapshot().settings);
    for (auto& output : s.outputs)
        if (output.id == device(2)) output.volumeDb = -12.0;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    send(apply);
    EXPECT_TRUE(waitFor([&](const Snapshot& now) { return at(now, 2) == -12.0; }));
    EXPECT_EQ(at(server->snapshot(), 1), -6.0);

    // Selected, its level is the settings' own.
    s = nativeSettings(server->snapshot().settings);
    selectOutput(s, Backend::Wasapi, device(2), server->snapshot().devices);
    apply.settings = settingsDto(s);
    const auto selected = send(apply);
    EXPECT_EQ(selected.settings.volume.db, -12.0);
    EXPECT_EQ(at(selected, 1), -6.0);
}

namespace {

Command transfer(const std::string& from, const std::string& to) {
    Command c;
    c.kind = CommandKind::Transfer;
    c.zone = from;
    c.target = to;
    return c;
}

} // namespace

// A transfer while playing: the queue, current entry and audible position go on in the other zone, and the zone left is
// empty and stopped with its device closed (MZ-R09).
TEST_F(ZonesTest, ATransferWhilePlayingPlaysOnInTheOtherZoneFromWhereItWas) {
    start(2);
    send(play(1, {track(1, 8.0), track(1, 1.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1), 1.5); }));
    const auto before = server->snapshot();
    auto s = send(transfer(zone(1), zone(2)));
    EXPECT_FALSE(s.commandError);
    ASSERT_TRUE(waitFor([](const Snapshot& now) { return playing(of(now, 2)); }));
    s = server->snapshot();
    EXPECT_EQ(of(s, 2).queue.entries, of(before, 1).queue.entries);
    EXPECT_EQ(of(s, 2).queueEntryId, of(before, 1).queueEntryId);
    EXPECT_GE(of(s, 2).playback.position + 4410, of(before, 1).playback.position);
    EXPECT_LT(of(s, 2).playback.position, of(before, 1).playback.position + uint64_t{3} * 44100);
    EXPECT_TRUE(of(s, 1).queue.entries.empty());
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Stopped);
    EXPECT_EQ(closed(1), opened(1));
    EXPECT_EQ(commandFromJson(toJson(transfer(zone(1), zone(2)))), transfer(zone(1), zone(2)));
}

TEST_F(ZonesTest, ATransferWhilePausedStaysPausedAtItsPosition) {
    start(2);
    send(play(1, {track(1, 8.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1), 1.0); }));
    send(command(CommandKind::Pause, 1));
    const auto position = of(server->snapshot(), 1).playback.position;
    auto s = send(transfer(zone(1), zone(2)));
    EXPECT_EQ(of(s, 2).playback.state, PlaybackState::Paused);
    EXPECT_NEAR(double(of(s, 2).playback.position), double(position), 4410.0);
    EXPECT_EQ(of(s, 1).playback.state, PlaybackState::Stopped);
    send(command(CommandKind::Play, 2));
    EXPECT_TRUE(waitFor([&](const Snapshot& now) {
        return now.zones.size() == 2 && playing(of(now, 2)) && of(now, 2).playback.position + 4410 >= position;
    }));
}

// The target's own queue is replaced; saved first as a playlist, it is kept there.
TEST_F(ZonesTest, ATransferOntoAZoneWithAQueueReplacesItAndAQueueSavedFirstStaysAPlaylist) {
    start(3, {}, true);
    send(play(1, {track(1, 8.0)}));
    send(play(2, {track(2, 3.0), track(2, 2.0)}));
    send(command(CommandKind::Stop, 2));
    send(play(3, {track(3, 3.0)}));
    send(command(CommandKind::Stop, 3));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)); }));
    const auto saved = server->saveQueueAsPlaylist("Zone 2's queue", zone(2));
    const auto ones = of(server->snapshot(), 1).queue.entries;
    auto s = send(transfer(zone(1), zone(2)));
    EXPECT_EQ(of(s, 2).queue.entries, ones);
    s = send(transfer(zone(2), zone(3)));
    EXPECT_EQ(of(s, 3).queue.entries, ones);
    EXPECT_TRUE(of(s, 2).queue.entries.empty());
    const auto playlists = server->playlists();
    const auto kept = std::ranges::find(playlists, saved.playlist, &PlaylistSummary::id);
    ASSERT_NE(kept, playlists.end());
    EXPECT_EQ(kept->tracks, 2);
}

TEST_F(ZonesTest, ATransferOntoADeviceAnotherZoneHoldsIsRefused) {
    localDevices = daveAndCirrus();
    start(0, [](Settings& s) {
        outputsOf(s,
                  {enabled(Backend::Wasapi, "w-dave", "DAVE"), enabled(Backend::Ks, "k-dave", "DAVE KS"),
                   enabled(Backend::Wasapi, "w-cirrus", "Cirrus")},
                  Backend::Wasapi, "w-cirrus");
    });
    send(refresh());
    const auto wDave = zoneId(Backend::Wasapi, "w-dave"), kDave = zoneId(Backend::Ks, "k-dave"),
               cirrus = zoneId(Backend::Wasapi, "w-cirrus");
    send(playIn(wDave, track(1, 6.0)));
    send(playIn(cirrus, track(2, 6.0)));
    ASSERT_TRUE(waitFor([&](const Snapshot& s) { return playing(in(s, wDave)) && playing(in(s, cirrus)); }));
    const auto before = in(server->snapshot(), cirrus).queue.entries;
    auto s = send(transfer(cirrus, kDave));
    ASSERT_TRUE(s.commandError);
    EXPECT_EQ(s.commandError->code, ErrorCode::DeviceInUse);
    EXPECT_EQ(s.commandError->detail, "in use by " + in(s, wDave).name);
    EXPECT_EQ(in(s, cirrus).queue.entries, before);
    EXPECT_EQ(in(s, cirrus).playback.state, PlaybackState::Playing);
    // The zone holding the device may move its own queue onto another backend of it.
    s = send(transfer(wDave, kDave));
    EXPECT_FALSE(s.commandError);
    EXPECT_TRUE(waitFor([&](const Snapshot& now) { return playing(in(now, kDave)); }));
}

namespace {

// Whether some program keeps Windows from sleeping now.
bool systemKeptAwake() {
    EXECUTION_STATE state = 0;
    CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof(state));
    return (state & ES_SYSTEM_REQUIRED) != 0;
}

} // namespace

// While any zone plays Windows does not sleep; paused and stopped zones let it (MAC-R27).
TEST_F(ZonesTest, WindowsIsKeptFromSleepingWhileAnyZonePlays) {
    const bool before = systemKeptAwake(); // another program may keep it awake already
    start(2);
    send(play(1, {track(1, 8.0)}));
    send(play(2, {track(2, 8.0)}));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return playing(of(s, 1)) && playing(of(s, 2)); }));
    EXPECT_TRUE(waitFor([](const Snapshot&) { return systemKeptAwake(); }));
    send(command(CommandKind::Pause, 1));
    EXPECT_TRUE(systemKeptAwake()) << "zone 2 still plays";
    Command all;
    all.kind = CommandKind::PauseAll;
    send(all);
    if (!before) EXPECT_TRUE(waitFor([](const Snapshot&) { return !systemKeptAwake(); }));
}
