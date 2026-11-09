// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "Common/util/Text.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

constexpr uint64_t kRate = 44100;

DeviceCatalog catalog() {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.wasapi = {{"w-speakers", "Speakers"}, {"w-dave", "DAVE"}};
    devices.asio = {{"a-dave", "ASIO Chord"}};
    devices.ks = {{"k-dave", "Chord KS"}};
    devices.defaultWasapi = "w-speakers";
    return devices;
}

Settings fresh() {
    Settings s;
    syncOutputs(s, catalog());
    return s;
}

const OutputRow& row(const std::vector<OutputRow>& rows, Backend backend, const std::string& id) {
    const auto found = std::ranges::find_if(rows, [&](const OutputRow& r) { return r.backend == backend && r.id == id; });
    if (found == rows.end()) throw std::logic_error("no row " + id);
    return *found;
}

const SavedOutput* saved(const Settings& s, Backend backend, const std::string& id) {
    const auto found = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    return found == s.outputs.end() ? nullptr : &*found;
}

// settings.json as 0.2.3 wrote it.
std::string earlierSettings(const char* backend, const char* wasapiDevice, const char* asioDriver, const char* ksDevice) {
    return std::string(R"({"schema":3,"backend":")") + backend +
           R"(","volume":{"mode":"software","db":-12.0},"wasapi":{"device":")" + wasapiDevice +
           R"(","shareMode":"shared"},"asio":{"driver":")" + asioDriver + R"(","bufferSize":512},"ks":{"device":")" + ksDevice +
           R"(","packetCount":8}})";
}

} // namespace

TEST(OutputsTest, ListsEveryDeviceByBackendWithTheDefaultFirst) {
    const auto rows = outputRows(fresh(), catalog());
    ASSERT_EQ(rows.size(), 5U);
    EXPECT_EQ(rows[0].id, "");
    EXPECT_EQ(rows[0].backend, Backend::Wasapi);
    EXPECT_EQ(rows[1].name, "Speakers");
    EXPECT_EQ(rows[2].name, "DAVE");
    EXPECT_EQ(rows[3].backend, Backend::Asio);
    EXPECT_EQ(rows[4].backend, Backend::Ks);
}

TEST(OutputsTest, NewInstallationEnablesAndSelectsTheWindowsDefault) {
    const auto rows = outputRows(fresh(), catalog());
    EXPECT_TRUE(rows[0].enabled);
    EXPECT_TRUE(rows[0].selected);
    EXPECT_FALSE(rows[0].saved); // not a device, so nothing to delete
    EXPECT_EQ(std::ranges::count_if(rows, [](const OutputRow& r) { return r.enabled; }), 1);
}

TEST(OutputsTest, RenamingShowsTheUsersNameAndAnEmptyNameRestoresTheDevices) {
    Settings s = fresh();
    renameOutput(s, Backend::Wasapi, "w-dave", "Desk DAC", catalog());
    EXPECT_EQ(row(outputRows(s, catalog()), Backend::Wasapi, "w-dave").name, "Desk DAC");
    renameOutput(s, Backend::Wasapi, "w-dave", "", catalog());
    EXPECT_EQ(row(outputRows(s, catalog()), Backend::Wasapi, "w-dave").name, "DAVE");
}

TEST(OutputsTest, EnablingListsADeviceAndTheOutputInUseStaysEnabled) {
    Settings s = fresh();
    setOutputEnabled(s, Backend::Asio, "a-dave", true, catalog());
    EXPECT_TRUE(row(outputRows(s, catalog()), Backend::Asio, "a-dave").enabled);
    setOutputEnabled(s, Backend::Asio, "a-dave", false, catalog());
    EXPECT_FALSE(row(outputRows(s, catalog()), Backend::Asio, "a-dave").enabled);
    setOutputEnabled(s, Backend::Wasapi, "", false, catalog());
    EXPECT_TRUE(row(outputRows(s, catalog()), Backend::Wasapi, "").enabled);
}

TEST(OutputsTest, TwoDevicesOfOneBackendKeepDifferentParameters) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    s.wasapi.shared = true;
    s.wasapi.fixedRate = 96000;
    s.volume.mode = VolumeMode::Software;
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Wasapi, "w-speakers", catalog());
    EXPECT_EQ(s.wasapi.deviceId, "w-speakers");
    EXPECT_FALSE(s.wasapi.shared);
    EXPECT_EQ(s.wasapi.fixedRate, 0U);
    EXPECT_EQ(s.volume.mode, VolumeMode::Fixed);
    s.wasapi.timing.predecodeSeconds = 30;
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_EQ(s.wasapi.deviceId, "w-dave");
    EXPECT_TRUE(s.wasapi.shared);
    EXPECT_EQ(s.wasapi.fixedRate, 96000U);
    EXPECT_EQ(s.wasapi.timing.predecodeSeconds, 10U);
    EXPECT_EQ(s.volume.mode, VolumeMode::Software);

    selectOutput(s, Backend::Wasapi, "w-speakers", catalog());
    EXPECT_EQ(s.wasapi.timing.predecodeSeconds, 30U);
}

TEST(OutputsTest, ADeviceChosenForTheFirstTimeStartsFromTheDefaults) {
    Settings s = fresh();
    s.asio.bufferSize = 512; // left from another driver
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(s.backend, Backend::Asio);
    EXPECT_EQ(s.asio.driverClsid, "a-dave");
    EXPECT_EQ(s.asio.bufferSize, 0);
    EXPECT_TRUE(row(outputRows(s, catalog()), Backend::Asio, "a-dave").selected);
}

TEST(OutputsTest, ParametersSurviveAFullRoundTripThroughTheFile) {
    Settings s = fresh();
    selectOutput(s, Backend::Ks, "k-dave", catalog());
    s.ks.packetCount = 8;
    syncOutputs(s, catalog());
    renameOutput(s, Backend::Ks, "k-dave", "聽音間", catalog());
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    s.asio.bufferSize = 1024;
    syncOutputs(s, catalog());

    Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.outputs, s.outputs);
    selectOutput(read, Backend::Ks, "k-dave", catalog());
    EXPECT_EQ(read.ks.packetCount, 8);
    EXPECT_EQ(row(outputRows(read, catalog()), Backend::Ks, "k-dave").name, "聽音間");
    selectOutput(read, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(read.asio.bufferSize, 1024);
}

TEST(OutputsTest, FollowingTheDefaultUsesTheSetOfTheCurrentDefaultDevice) {
    Settings s = fresh();
    EXPECT_EQ(s.followedDefault, "w-speakers");
    s.wasapi.shared = true;
    syncOutputs(s, catalog());

    auto moved = catalog();
    moved.revision = 2;
    moved.defaultWasapi = "w-dave";
    syncOutputs(s, moved);
    EXPECT_EQ(s.followedDefault, "w-dave");
    EXPECT_EQ(s.wasapi.deviceId, "");
    EXPECT_FALSE(s.wasapi.shared);
    s.wasapi.fixedRate = 192000;
    syncOutputs(s, moved);

    syncOutputs(s, catalog());
    EXPECT_EQ(s.followedDefault, "w-speakers");
    EXPECT_TRUE(s.wasapi.shared);
    EXPECT_EQ(s.wasapi.fixedRate, 0U);

    syncOutputs(s, moved);
    EXPECT_EQ(s.wasapi.fixedRate, 192000U);
}

TEST(OutputsTest, ADefaultDeviceThatChangedWhileClosedIsFollowedAtTheFirstScan) {
    Settings s = fresh();
    s.wasapi.shared = true;
    syncOutputs(s, catalog());
    Settings read = fromJson(toJson(s));

    auto moved = catalog();
    moved.defaultWasapi = "w-dave";
    syncOutputs(read, moved);
    EXPECT_FALSE(read.wasapi.shared);
    EXPECT_TRUE(saved(read, Backend::Wasapi, "w-speakers")->wasapi.shared);
}

TEST(OutputsTest, ADeviceChosenDirectlyDoesNotFollowTheDefault) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    s.wasapi.shared = true;
    auto moved = catalog();
    moved.defaultWasapi = "w-dave";
    syncOutputs(s, moved);
    EXPECT_EQ(s.followedDefault, "");
    EXPECT_TRUE(s.wasapi.shared);
    EXPECT_EQ(s.wasapi.deviceId, "w-dave");
}

TEST(OutputsTest, UpgradeGivesEachBackendsSetToItsDeviceAndEnablesTheOneInUse) {
    Settings s = fromJson(earlierSettings("asio", "w-dave", "a-dave", "k-dave"));
    EXPECT_TRUE(s.adoptOutputSets);
    syncOutputs(s, catalog());
    EXPECT_FALSE(s.adoptOutputSets);

    EXPECT_EQ(s.backend, Backend::Asio);
    EXPECT_EQ(s.asio.bufferSize, 512);
    EXPECT_TRUE(saved(s, Backend::Asio, "a-dave")->enabled);
    EXPECT_FALSE(saved(s, Backend::Wasapi, "w-dave")->enabled);
    EXPECT_FALSE(saved(s, Backend::Ks, "k-dave")->enabled);

    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_TRUE(s.wasapi.shared);
    EXPECT_EQ(s.volume.mode, VolumeMode::Software);
    selectOutput(s, Backend::Ks, "k-dave", catalog());
    EXPECT_EQ(s.ks.packetCount, 8);
    selectOutput(s, Backend::Wasapi, "w-speakers", catalog());
    EXPECT_FALSE(s.wasapi.shared); // other devices start from the defaults
}

TEST(OutputsTest, UpgradeGivesTheFollowedSetToTheDefaultDevice) {
    Settings s = fromJson(earlierSettings("wasapi", "", "", ""));
    syncOutputs(s, catalog());
    EXPECT_TRUE(s.wasapi.shared);
    EXPECT_EQ(s.followedDefault, "w-speakers");
    EXPECT_TRUE(saved(s, Backend::Wasapi, "")->enabled);
    EXPECT_TRUE(saved(s, Backend::Wasapi, "w-speakers")->wasapi.shared);
    EXPECT_FALSE(saved(s, Backend::Wasapi, "w-speakers")->enabled);
}

TEST(OutputsTest, UpgradeGivesAnEmptyChoiceToTheFirstDriverOrPin) {
    Settings s = fromJson(earlierSettings("ks", "", "", ""));
    syncOutputs(s, catalog());
    EXPECT_EQ(s.ks.deviceId, "k-dave");
    EXPECT_EQ(s.ks.packetCount, 8);
    EXPECT_TRUE(saved(s, Backend::Ks, "k-dave")->enabled);
    EXPECT_EQ(saved(s, Backend::Asio, "a-dave")->asio.bufferSize, 512);
}

TEST(OutputsTest, UpgradeWaitsForTheFirstDeviceScanAndSurvivesARestartBeforeIt) {
    Settings s = fromJson(earlierSettings("wasapi", "", "", ""));
    syncOutputs(s, DeviceCatalog{});
    EXPECT_TRUE(s.adoptOutputSets);
    EXPECT_TRUE(s.outputs.empty());
    EXPECT_TRUE(fromJson(toJson(s)).adoptOutputSets);
}

TEST(OutputsTest, DeletingAConnectedDeviceLeavesItListedWithTheDefaults) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    s.wasapi.shared = true;
    syncOutputs(s, catalog());
    renameOutput(s, Backend::Wasapi, "w-dave", "Desk DAC", catalog());
    selectOutput(s, Backend::Wasapi, "", catalog());

    deleteOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_EQ(saved(s, Backend::Wasapi, "w-dave"), nullptr);
    const auto rows = outputRows(s, catalog());
    EXPECT_EQ(row(rows, Backend::Wasapi, "w-dave").name, "DAVE");
    EXPECT_FALSE(row(rows, Backend::Wasapi, "w-dave").enabled);
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_FALSE(s.wasapi.shared);
}

TEST(OutputsTest, DeletingAMissingDeviceRemovesItFromTheList) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    selectOutput(s, Backend::Wasapi, "", catalog());
    auto without = catalog();
    without.wasapi.pop_back();
    syncOutputs(s, without);
    const auto rows = outputRows(s, without);
    EXPECT_FALSE(row(rows, Backend::Wasapi, "w-dave").connected);
    EXPECT_EQ(row(rows, Backend::Wasapi, "w-dave").name, "DAVE");

    deleteOutput(s, Backend::Wasapi, "w-dave", without);
    EXPECT_EQ(rows.size() - 1, outputRows(s, without).size());
}

TEST(OutputsTest, DeletingTheOutputInUseKeepsItSelectedWithTheDefaults) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    s.asio.bufferSize = 512;
    syncOutputs(s, catalog());
    deleteOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(s.backend, Backend::Asio);
    EXPECT_EQ(s.asio.driverClsid, "a-dave");
    EXPECT_EQ(s.asio.bufferSize, 0);
    EXPECT_TRUE(saved(s, Backend::Asio, "a-dave")->enabled);
}

TEST(OutputsTest, TheWindowsDefaultEntryCannotBeDeleted) {
    Settings s = fresh();
    deleteOutput(s, Backend::Wasapi, "", catalog());
    EXPECT_TRUE(saved(s, Backend::Wasapi, "")->enabled);
}

TEST(OutputsTest, ADeviceThatDisappearsStaysSelectedAndIsNeverSubstituted) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    s.wasapi.shared = true;
    auto without = catalog();
    without.revision = 2;
    without.wasapi.pop_back();
    syncOutputs(s, without);
    EXPECT_EQ(s.wasapi.deviceId, "w-dave");
    EXPECT_TRUE(s.wasapi.shared);
    const auto rows = outputRows(s, without);
    EXPECT_TRUE(row(rows, Backend::Wasapi, "w-dave").selected);
    EXPECT_FALSE(row(rows, Backend::Wasapi, "w-dave").connected);

    syncOutputs(s, catalog());
    EXPECT_TRUE(row(outputRows(s, catalog()), Backend::Wasapi, "w-dave").connected);
    EXPECT_TRUE(s.wasapi.shared);
}

TEST(OutputsTest, NetworkPlayersAreListedKeptAndFoundAgainByUdn) {
    auto devices = catalog();
    devices.network = {{"uuid:eversolo", "Eversolo DMP-A6"}};
    Settings s = fresh();
    setOutputEnabled(s, Backend::Network, "uuid:eversolo", true, devices);
    const auto rows = outputRows(s, devices);
    ASSERT_EQ(rows.back().backend, Backend::Network);
    EXPECT_EQ(rows.back().name, "Eversolo DMP-A6");
    EXPECT_TRUE(rows.back().enabled);

    selectOutput(s, Backend::Network, "uuid:eversolo", devices);
    EXPECT_EQ(s.backend, Backend::Network);
    EXPECT_EQ(s.network.udn, "uuid:eversolo");
    s.network.timing.formatChangeSilenceMs = 500;
    syncOutputs(s, devices);

    auto gone = devices;
    gone.revision = 2;
    gone.network.clear();
    syncOutputs(s, gone);
    const auto missing = outputRows(s, gone);
    EXPECT_FALSE(missing.back().connected);
    EXPECT_TRUE(missing.back().selected);
    EXPECT_EQ(missing.back().name, "Eversolo DMP-A6");

    Settings read = fromJson(toJson(s));
    selectOutput(read, Backend::Wasapi, "", devices);
    selectOutput(read, Backend::Network, "uuid:eversolo", devices);
    EXPECT_EQ(read.network.timing.formatChangeSilenceMs, 500U);
}

TEST(OutputsTest, ANetworkPlayerStartsOnItsOwnVolumeAndKeepsEachPlayersChoices) {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.network = {{"uuid:a", "A"}, {"uuid:b", "B"}};
    Settings s;
    setOutputEnabled(s, Backend::Network, "uuid:a", true, devices);
    selectOutput(s, Backend::Network, "uuid:a", devices);
    EXPECT_EQ(s.volume.mode, VolumeMode::Player);
    EXPECT_EQ(s.network.timing.releaseAfterPauseSeconds, 60U);
    EXPECT_TRUE(s.network.gapless);
    s.volume.mode = VolumeMode::Fixed;
    s.network.maxSampleRate = 96000;
    s.network.maxBits = 16;
    s.network.dsdOutput = DsdOutput::Pcm;
    s.network.dsdPcmRate = 88200;
    s.network.gapless = false;
    selectOutput(s, Backend::Network, "uuid:b", devices);
    EXPECT_EQ(s.volume.mode, VolumeMode::Player);
    EXPECT_EQ(s.network.maxSampleRate, 0U);
    EXPECT_TRUE(s.network.gapless);

    Settings read = fromJson(toJson(s));
    selectOutput(read, Backend::Network, "uuid:a", devices);
    EXPECT_EQ(read.volume.mode, VolumeMode::Fixed);
    EXPECT_EQ(read.network.maxSampleRate, 96000U);
    EXPECT_EQ(read.network.maxBits, 16U);
    EXPECT_EQ(read.network.dsdOutput, DsdOutput::Pcm);
    EXPECT_EQ(read.network.dsdPcmRate, 88200U);
    EXPECT_FALSE(read.network.gapless);
}

TEST(OutputsTest, ANetworkPlayerNeverKeepsSoftwareVolume) {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.network = {{"uuid:a", "A"}};
    devices.wasapi = {{"device", "Device"}};
    Settings s;
    selectOutput(s, Backend::Wasapi, "device", devices);
    s.volume.mode = VolumeMode::Software;
    selectOutput(s, Backend::Network, "uuid:a", devices);
    EXPECT_EQ(s.volume.mode, VolumeMode::Player);
    s.outputs.back().volumeMode = VolumeMode::Software;
    selectOutput(s, Backend::Wasapi, "device", devices);
    EXPECT_EQ(s.volume.mode, VolumeMode::Software);
    selectOutput(s, Backend::Network, "uuid:a", devices);
    EXPECT_EQ(s.volume.mode, VolumeMode::Player);
}

TEST(OutputsServerTest, PlayingToANetworkPlayerFailsWithoutSubstitutingADevice) {
    Settings s = fresh();
    s.autoplay = false;
    auto devices = catalog();
    devices.network = {{"uuid:player", "Player"}};
    selectOutput(s, Backend::Network, "uuid:player", devices);
    LocalServer server(s);
    Command replace;
    replace.kind = CommandKind::QueueReplace;
    replace.sources = {TrackSource(toUtf8(testutil::testFile("24-96k").wstring()))};
    server.submit(std::move(replace));
    server.waitIdle();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!server.snapshot().playback.error && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    const auto playback = server.snapshot().playback;
    ASSERT_TRUE(playback.error);
    EXPECT_EQ(*playback.error, audio::ErrorCode::DeviceNotFound);
    EXPECT_NE(playback.state, PlaybackState::Playing);
    EXPECT_EQ(server.snapshot().settings.backend, Backend::Network);
    server.shutdown();
}

TEST(OutputsTest, AnOutputChangedForTheFirstTimeSurvivesTheServersCheckOfTheFile) {
    auto devices = catalog();
    devices.network = {{"uuid:player", "Player"}};
    for (const auto& [backend, id] : std::vector<std::pair<Backend, std::string>>{{Backend::Wasapi, "w-dave"},
                                                                                  {Backend::Asio, "a-dave"},
                                                                                  {Backend::Ks, "k-dave"},
                                                                                  {Backend::Network, "uuid:player"}}) {
        Settings enabled = fresh();
        setOutputEnabled(enabled, backend, id, true, devices);
        EXPECT_EQ(fromJson(toJson(enabled)).outputs, enabled.outputs) << backendId(backend);
        Settings renamed = fresh();
        renameOutput(renamed, backend, id, "Mine", devices);
        EXPECT_EQ(fromJson(toJson(renamed)).outputs, renamed.outputs) << backendId(backend);
    }
}

TEST(OutputsServerTest, TheServerTakesAnOutputEnabledForTheFirstTime) {
    Settings s = fresh();
    s.autoplay = false;
    LocalServer server(s);
    setOutputEnabled(s, Backend::Wasapi, "w-dave", true, catalog());
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    EXPECT_NO_THROW(server.submit(std::move(apply)));
    server.shutdown();
}

TEST(OutputsTest, ResettingPlaybackForgetsTheDevices) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    Settings reset = resetPlayback(s);
    syncOutputs(reset, catalog());
    EXPECT_EQ(reset.backend, Backend::Wasapi);
    EXPECT_EQ(saved(reset, Backend::Asio, "a-dave"), nullptr);
    EXPECT_TRUE(saved(reset, Backend::Wasapi, "")->enabled);
}

namespace {

// Plays a queue of two tracks on capture devices and records which outputs the Server opens.
class OutputSwitchTest : public ::testing::Test {
protected:
    OutputSwitchTest()
        : server_(start(), {}, [this](const Settings& s) {
              {
                  std::scoped_lock lock(mutex_);
                  opened_.push_back(s.backend);
              }
              audio::CaptureOptions options;
              options.file = testutil::tempDir() / "output-switch.raw";
              options.realtime = true;
              return audio::createCaptureOutput(options);
          }) {
        const auto first = testutil::generatedFile({"output-switch-first", 44100, 16, 2, 6.0});
        const auto second = testutil::generatedFile({"output-switch-second", 44100, 16, 2, 1.0});
        Command replace;
        replace.kind = CommandKind::QueueReplace;
        replace.sources = {TrackSource(toUtf8(first.wstring())), TrackSource(toUtf8(second.wstring()))};
        server_.submit(std::move(replace));
    }

    ~OutputSwitchTest() override { server_.shutdown(); }

    static Settings start() {
        Settings s = fresh();
        s.autoplay = false;
        return s;
    }

    bool waitFor(const std::function<bool(const Snapshot&)>& check) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (check(server_.snapshot())) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    void waitUntilPlaying() {
        ASSERT_TRUE(
            waitFor([](const Snapshot& s) { return s.playback.state == PlaybackState::Playing && s.playback.position > 22050; }));
    }

    void switchTo(Backend backend, const std::string& id) {
        Settings s = nativeSettings(server_.snapshot().settings);
        selectOutput(s, backend, id, catalog());
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(s);
        server_.submit(std::move(apply));
        server_.waitIdle();
    }

    std::vector<Backend> opened() {
        std::scoped_lock lock(mutex_);
        return opened_;
    }

    std::mutex mutex_;
    std::vector<Backend> opened_;
    LocalServer server_;
};

} // namespace

TEST_F(OutputSwitchTest, WhilePlayingContinuesOnTheNewOutputFromTheAudiblePosition) {
    waitUntilPlaying();
    const Snapshot before = server_.snapshot();
    switchTo(Backend::Asio, "a-dave");
    ASSERT_TRUE(waitFor([](const Snapshot& s) {
        return s.playback.state == PlaybackState::Playing && s.playback.activeBackend == Backend::Asio;
    }));
    const Snapshot after = server_.snapshot();
    EXPECT_EQ(opened().back(), Backend::Asio);
    EXPECT_FALSE(after.playback.error) << after.playback.errorDetail;
    EXPECT_GE(after.playback.position, before.playback.position - 4410);
    EXPECT_LT(after.playback.position, before.playback.position + 2 * kRate);
    EXPECT_EQ(after.queueEntryId, before.queueEntryId);
    EXPECT_EQ(after.queue.entries, before.queue.entries);
}

TEST_F(OutputSwitchTest, TheQueueGoesOnToTheNextTrackOnTheNewOutput) {
    waitUntilPlaying();
    const auto entries = server_.snapshot().queue.entries;
    ASSERT_EQ(entries.size(), 2U);
    switchTo(Backend::Ks, "k-dave");
    Command seek;
    seek.kind = CommandKind::Seek;
    seek.frame = 5 * kRate;
    server_.submit(std::move(seek));
    const uint64_t second = entries[1].id;
    EXPECT_TRUE(waitFor([second](const Snapshot& s) { return s.queueEntryId == second; }));
}

TEST_F(OutputSwitchTest, WhilePausedStaysPausedAtItsPosition) {
    waitUntilPlaying();
    Command pause;
    pause.kind = CommandKind::Pause;
    server_.submit(std::move(pause));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return s.playback.state == PlaybackState::Paused; }));
    const uint64_t position = server_.snapshot().playback.position;
    switchTo(Backend::Asio, "a-dave");
    const Snapshot after = server_.snapshot();
    EXPECT_EQ(after.playback.state, PlaybackState::Paused);
    EXPECT_EQ(after.settings.backend, Backend::Asio);
    EXPECT_NEAR(double(after.playback.position), double(position), 4410.0);

    Command play;
    play.kind = CommandKind::Play;
    server_.submit(std::move(play));
    EXPECT_TRUE(waitFor([](const Snapshot& s) {
        return s.playback.state == PlaybackState::Playing && s.playback.activeBackend == Backend::Asio;
    }));
}

TEST_F(OutputSwitchTest, BetweenTwoDevicesOfOneBackendOpensTheNewDevice) {
    waitUntilPlaying();
    const size_t opens = opened().size();
    switchTo(Backend::Wasapi, "w-dave");
    ASSERT_TRUE(
        waitFor([&](const Snapshot& s) { return s.playback.state == PlaybackState::Playing && opened().size() > opens; }));
    EXPECT_EQ(server_.snapshot().settings.wasapi.deviceId, "w-dave");
}

TEST(OutputsServerTest, TheFirstDeviceScanEnablesTheOutputInUseAndSavesIt) {
    const auto file = testutil::tempDir() / "outputs-settings" / "settings.json";
    std::filesystem::remove_all(file.parent_path());
    std::filesystem::create_directories(file.parent_path());
    {
        Settings s;
        s.autoplay = false;
        LocalServer server(s, file);
        Command refresh;
        refresh.kind = CommandKind::RefreshDevices;
        server.submit(std::move(refresh));
        server.waitIdle();
        const auto outputs = server.snapshot().settings.outputs;
        EXPECT_NE(std::ranges::find_if(outputs, [](const SavedOutput& o) { return o.id.empty() && o.enabled; }), outputs.end());
        server.shutdown();
    }
    const Settings read = loadSettings(file);
    EXPECT_NE(std::ranges::find_if(read.outputs, [](const SavedOutput& o) { return o.id.empty() && o.enabled; }),
              read.outputs.end());
}

TEST(OutputParametersTest, AnOutputNotInUseIsEditedWithoutLeavingTheOneInUse) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    s.asio.outLeft = 3;
    syncOutputs(s, catalog());

    Settings edited = withOutputParameters(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_EQ(edited.backend, Backend::Wasapi);
    edited.wasapi.customPeriodMs = 33.0;
    keepOutputParameters(s, edited, Backend::Wasapi, "w-dave", catalog());

    EXPECT_EQ(s.backend, Backend::Asio);
    EXPECT_EQ(s.asio.driverClsid, "a-dave");
    EXPECT_EQ(s.asio.outLeft, 3);
    EXPECT_NE(s.wasapi.customPeriodMs, 33.0);
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_EQ(s.wasapi.customPeriodMs, 33.0);
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(s.asio.outLeft, 3);
}

TEST(OutputParametersTest, TheOutputInUseChangesAtOnce) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    Settings edited = withOutputParameters(s, Backend::Asio, "a-dave", catalog());
    edited.asio.outLeft = 5;
    keepOutputParameters(s, edited, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(s.asio.outLeft, 5);
}

TEST(OutputParametersTest, TheWindowsDefaultEntryKeepsThemUnderTheDeviceItFollows) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    Settings edited = withOutputParameters(s, Backend::Wasapi, "", catalog());
    EXPECT_TRUE(edited.wasapi.deviceId.empty());
    edited.wasapi.customPeriodMs = 27.0;
    keepOutputParameters(s, edited, Backend::Wasapi, "", catalog());
    EXPECT_EQ(withOutputParameters(s, Backend::Wasapi, "", catalog()).wasapi.customPeriodMs, 27.0);
    EXPECT_EQ(s.backend, Backend::Asio);
}

namespace {

DeviceCatalog withReceivers() {
    DeviceCatalog devices = catalog();
    devices.airplay = {{"HP-A", "Bedroom"}, {"HP-B", "Bedroom (2)"}, {"PAIR", "Living room", {}, {"P-1", "P-2"}}};
    return devices;
}

std::vector<std::string> group(const Settings& s) {
    return s.airplayGroup;
}

} // namespace

TEST(AirPlayGroupTest, ChoosingAReceiverStartsAGroupOfOne) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    const auto rows = outputRows(s, withReceivers());
    EXPECT_TRUE(row(rows, Backend::AirPlay, "HP-A").grouped);
    EXPECT_FALSE(row(rows, Backend::AirPlay, "HP-B").grouped);
    EXPECT_TRUE(row(rows, Backend::AirPlay, "PAIR").pair);
    EXPECT_FALSE(row(rows, Backend::AirPlay, "HP-A").pair);
}

TEST(AirPlayGroupTest, ReceiversAreAddedAndTakenOutTheLastOneStaying) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A", "HP-B"}));
    EXPECT_TRUE(saved(s, Backend::AirPlay, "HP-B")->enabled);
    EXPECT_TRUE(row(outputRows(s, withReceivers()), Backend::AirPlay, "HP-B").grouped);
    setInAirPlayGroup(s, "HP-B", false, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    setInAirPlayGroup(s, "HP-A", false, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    EXPECT_EQ(s.airplay.deviceId, "HP-A");
}

TEST(AirPlayGroupTest, TheReceiverInUseLeavingHandsOverToTheNext) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    s.airplay.channels = AirPlayChannels::Left;
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    setInAirPlayGroup(s, "HP-A", false, withReceivers());
    EXPECT_EQ(s.backend, Backend::AirPlay);
    EXPECT_EQ(s.airplay.deviceId, "HP-B");
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-B"}));
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-A")->airplay.channels, AirPlayChannels::Left);
    EXPECT_EQ(airplayChannels(s, "HP-A"), AirPlayChannels::Left);
    EXPECT_EQ(airplayChannels(s, "HP-B"), AirPlayChannels::Stereo);
}

TEST(AirPlayGroupTest, ChoosingAnyReceiverOfTheLastGroupBringsItBack) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    selectOutput(s, Backend::Asio, "a-dave", withReceivers());
    EXPECT_FALSE(row(outputRows(s, withReceivers()), Backend::AirPlay, "HP-B").grouped);
    selectOutput(s, Backend::AirPlay, "HP-B", withReceivers());
    EXPECT_EQ(s.airplay.deviceId, "HP-B");
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A", "HP-B"}));
    selectOutput(s, Backend::Asio, "a-dave", withReceivers());
    selectOutput(s, Backend::AirPlay, "PAIR", withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"PAIR"}));
}

TEST(AirPlayGroupTest, AReceiverDeletedLeavesTheGroup) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    deleteOutput(s, Backend::AirPlay, "HP-B", withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
}

TEST(AirPlayGroupTest, ChannelsAndTheGroupSurviveInTheSettingsFile) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    s.airplay.channels = AirPlayChannels::Right;
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    const Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.airplayGroup, (std::vector<std::string>{"HP-A", "HP-B"}));
    EXPECT_EQ(read.airplay.channels, AirPlayChannels::Right);
    EXPECT_EQ(saved(read, Backend::AirPlay, "HP-A")->airplay.channels, AirPlayChannels::Right);
    EXPECT_TRUE(fromJson(toJson(Settings{})).airplayGroup.empty());
    EXPECT_EQ(fromJson(toJson(Settings{})).airplay.channels, AirPlayChannels::Stereo);
}
