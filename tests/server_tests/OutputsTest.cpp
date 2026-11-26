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
#include "ControlPoint/OutputsFollow.h"
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

// A player that fetches the stream from another computer goes through the firewall rule; an HQPlayer on this computer
// reaches it on the loopback.
TEST(OutputsTest, PlayersAndAnHQPlayerOnAnotherComputerNeedTheFirewallRule) {
    const auto output = [](Backend backend, std::string id) {
        OutputRow r;
        r.backend = backend;
        r.id = std::move(id);
        return r;
    };
    EXPECT_TRUE(needsFirewallRule(output(Backend::Network, "uuid:player")));
    EXPECT_TRUE(needsFirewallRule(output(Backend::AirPlay, "AABBCCDDEEFF")));
    EXPECT_TRUE(needsFirewallRule(output(Backend::HQPlayer, "192.0.2.10:4321")));
    EXPECT_TRUE(needsFirewallRule(output(Backend::HQPlayer, "macbook-air.local:4321")));
    EXPECT_FALSE(needsFirewallRule(output(Backend::HQPlayer, "localhost:4321")));
    EXPECT_FALSE(needsFirewallRule(output(Backend::HQPlayer, "127.0.0.1:4321")));
    EXPECT_FALSE(needsFirewallRule(output(Backend::Wasapi, "")));
    EXPECT_FALSE(needsFirewallRule(output(Backend::Asio, "a-dave")));
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

// The DMP-A6 came back on 2026-11-19 under a new UDN, and was listed twice, the saved one not connected for ever. A player
// gone that answers again under a new UDN with the same name and model is taken for it (the owner, 2026-11-19): what was
// saved for it, its use included, goes to the new UDN.
TEST(OutputsTest, APlayerBackUnderANewUdnKeepsWhatWasSavedForIt) {
    auto devices = catalog();
    devices.network = {{"uuid:old", "DMP-A6"}};
    devices.network[0].model = "EVERSOLO AV Renderer Device";
    Settings s = fresh();
    selectOutput(s, Backend::Network, "uuid:old", devices);
    s.network.timing.formatChangeSilenceMs = 500;
    renameOutput(s, Backend::Network, "uuid:old", "Living room", devices);
    const Settings read = fromJson(toJson(s));
    const SavedOutput* kept = saved(read, Backend::Network, "uuid:old");
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->model, "EVERSOLO AV Renderer Device");

    auto renewed = devices;
    renewed.revision = 2;
    renewed.network = {{"uuid:new", "DMP-A6"}};
    renewed.network[0].model = "EVERSOLO AV Renderer Device";
    syncOutputs(s, renewed);
    const auto rows = outputRows(s, renewed);
    EXPECT_EQ(std::ranges::count_if(rows, [](const OutputRow& r) { return r.backend == Backend::Network; }), 1);
    const auto& player = row(rows, Backend::Network, "uuid:new");
    EXPECT_TRUE(player.selected);
    EXPECT_TRUE(player.connected);
    EXPECT_EQ(player.name, "Living room");
    EXPECT_EQ(s.network.udn, "uuid:new");
    EXPECT_EQ(s.network.timing.formatChangeSilenceMs, 500U);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:old"), nullptr);
}

// One not in use is renewed whole: the window sends the settings back with every change, and the Server takes only what
// reads back as it was. The KECES's entry kept its old UDN for the other outputs' kinds, every change was refused, and the
// window ended with the first (2026-11-20).
TEST(OutputsTest, APlayerNotInUseBackUnderANewUdnReadsBackAsItIs) {
    auto devices = catalog();
    devices.network = {{"uuid:old", "E620"}};
    devices.network[0].model = "MUZO Cobblestone";
    Settings s = fresh();
    setOutputEnabled(s, Backend::Network, "uuid:old", true, devices);
    auto renewed = devices;
    renewed.revision = 2;
    renewed.network = {{"uuid:new", "E620"}};
    renewed.network[0].model = "MUZO Cobblestone";
    syncOutputs(s, renewed);
    ASSERT_NE(saved(s, Backend::Network, "uuid:new"), nullptr);
    EXPECT_EQ(settingsDto(fromJson(toJson(s))), settingsDto(s));
}

// A player of the same name but another model, or two new ones that both fit, are not taken for the saved one.
TEST(OutputsTest, APlayerOfAnotherModelOrTwoThatFitAreNotTakenForASavedOne) {
    auto devices = catalog();
    devices.network = {{"uuid:old", "Speaker"}};
    devices.network[0].model = "Maker A";
    Settings s = fresh();
    setOutputEnabled(s, Backend::Network, "uuid:old", true, devices);
    auto other = devices;
    other.revision = 2;
    other.network = {{"uuid:other", "Speaker"}};
    other.network[0].model = "Maker B";
    syncOutputs(s, other);
    EXPECT_NE(saved(s, Backend::Network, "uuid:old"), nullptr);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:other"), nullptr);
    auto two = devices;
    two.revision = 3;
    two.network = {{"uuid:x", "Speaker"}, {"uuid:y", "Speaker"}};
    two.network[0].model = two.network[1].model = "Maker A";
    syncOutputs(s, two);
    EXPECT_NE(saved(s, Backend::Network, "uuid:old"), nullptr);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:x"), nullptr);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:y"), nullptr);
}

// One saved before players' models were kept is known by its name alone, as a stereo pair saved before its speakers were.
TEST(OutputsTest, APlayerSavedBeforeModelsWereKeptIsKnownByItsName) {
    Settings s = fresh();
    SavedOutput old;
    old.backend = Backend::Network;
    old.id = old.network.udn = "uuid:old";
    old.name = "DMP-A6";
    old.enabled = true;
    old.volumeMode = VolumeMode::Player;
    s.outputs.push_back(old);
    auto devices = catalog();
    devices.network = {{"uuid:new", "DMP-A6"}};
    devices.network[0].model = "EVERSOLO AV Renderer Device";
    syncOutputs(s, devices);
    const SavedOutput* now = saved(s, Backend::Network, "uuid:new");
    ASSERT_NE(now, nullptr);
    EXPECT_TRUE(now->enabled);
    EXPECT_EQ(now->volumeMode, VolumeMode::Player);
    EXPECT_EQ(now->model, "EVERSOLO AV Renderer Device");
    EXPECT_EQ(saved(s, Backend::Network, "uuid:old"), nullptr);
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

// DoP chosen for a network player, even one that declares DSD, is kept as chosen.
TEST(OutputsTest, ANetworkPlayersDopIsKept) {
    Settings s;
    s.network.dsdOutput = DsdOutput::Dop;
    EXPECT_EQ(fromJson(toJson(s)).network.dsdOutput, DsdOutput::Dop);
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

// Saved outputs keep the order the last scan found them in, so the rows before the next scan are in the order after it
// (MZ-R07: the zone picker's rows do not move).
TEST(OutputsTest, TheRowsBeforeADeviceScanKeepTheLastScansOrder) {
    Settings s = fresh();
    setOutputEnabled(s, Backend::Wasapi, "w-dave", true, catalog());
    setOutputEnabled(s, Backend::Wasapi, "w-speakers", true, catalog()); // saved after the DAVE, found before it
    syncOutputs(s, catalog());
    const auto ids = [](const std::vector<OutputRow>& rows) {
        std::vector<std::string> enabled;
        for (const auto& r : rows)
            if (r.enabled && r.backend == Backend::Wasapi && !r.id.empty()) enabled.push_back(r.id);
        return enabled;
    };
    const auto scanned = ids(outputRows(s, catalog()));
    EXPECT_EQ(scanned, (std::vector<std::string>{"w-speakers", "w-dave"}));
    EXPECT_EQ(ids(outputRows(fromJson(toJson(s)), DeviceCatalog{})), scanned);
}

namespace {

SavedOutput savedPlayer(const std::string& udn, const std::string& name, const std::string& model, const std::string& mac) {
    SavedOutput o;
    o.backend = Backend::Network;
    o.id = o.network.udn = udn;
    o.name = name;
    o.model = model;
    o.mac = mac;
    o.enabled = true;
    return o;
}

DeviceInfo foundPlayer(const std::string& udn, const std::string& name, const std::string& model, const std::string& mac) {
    DeviceInfo d{udn, name};
    d.model = model;
    d.mac = mac;
    return d;
}

} // namespace

// Two players of one name and model: the one saved and gone is not taken for the other, never used, when their MACs differ
// (MZ-R14); one back under a new UDN with its MAC is followed whatever its name.
TEST(OutputsTest, APlayerUnderANewUdnIsFollowedByItsMacAndAnotherOfItsNameIsNot) {
    Settings s = fresh();
    s.outputs.push_back(savedPlayer("uuid:first", "DMP-A6", "Eversolo DMP-A6", "aa:aa:aa:aa:aa:01"));
    DeviceCatalog devices = catalog();
    devices.network = {foundPlayer("uuid:second", "DMP-A6", "Eversolo DMP-A6", "aa:aa:aa:aa:aa:02")};
    EXPECT_TRUE(syncOutputs(s, devices).empty());
    EXPECT_NE(saved(s, Backend::Network, "uuid:first"), nullptr);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:second"), nullptr);

    devices.network.push_back(foundPlayer("uuid:renewed", "Living room", "Eversolo DMP-A6", "aa:aa:aa:aa:aa:01"));
    const auto moves = syncOutputs(s, devices);
    EXPECT_EQ(moves, (std::vector<OutputMove>{{Backend::Network, "uuid:first", "uuid:renewed"}}));
    EXPECT_EQ(saved(s, Backend::Network, "uuid:first"), nullptr);
    ASSERT_NE(saved(s, Backend::Network, "uuid:renewed"), nullptr);
    EXPECT_TRUE(saved(s, Backend::Network, "uuid:renewed")->enabled);
    EXPECT_EQ(saved(s, Backend::Network, "uuid:renewed")->mac, "aa:aa:aa:aa:aa:01");
}

// Without a MAC, a player is followed by its name and model only while no other saved player shares them.
TEST(OutputsTest, WithoutAMacAPlayerIsFollowedByNameAndModelOnlyWhenNoOtherSavedOneSharesThem) {
    Settings s = fresh();
    s.outputs.push_back(savedPlayer("uuid:one", "KECES", "KECES E620", ""));
    DeviceCatalog devices = catalog();
    devices.network = {foundPlayer("uuid:new", "KECES", "KECES E620", "")};
    Settings shared = s;
    shared.outputs.push_back(savedPlayer("uuid:two", "KECES", "KECES E620", ""));
    EXPECT_TRUE(syncOutputs(shared, devices).empty());
    EXPECT_NE(saved(shared, Backend::Network, "uuid:one"), nullptr);
    EXPECT_NE(saved(shared, Backend::Network, "uuid:two"), nullptr);

    EXPECT_EQ(syncOutputs(s, devices), (std::vector<OutputMove>{{Backend::Network, "uuid:one", "uuid:new"}}));
    EXPECT_NE(saved(s, Backend::Network, "uuid:new"), nullptr);
}

// An AirPlay pair saved before its speakers were is followed by its name only when no other saved receiver has that name.
TEST(OutputsTest, AnAirPlayPairSavedBeforeItsSpeakersIsFollowedByANameNoOtherReceiverHas) {
    Settings s = fresh();
    SavedOutput pair;
    pair.backend = Backend::AirPlay;
    pair.id = pair.airplay.deviceId = "OLD-PAIR";
    pair.name = "Living Room";
    pair.enabled = true;
    s.outputs.push_back(pair);
    DeviceCatalog devices = catalog();
    DeviceInfo repaired{"NEW-PAIR", "Living Room"};
    repaired.speakers = {"NEW-PAIR", "SPEAKER-2"};
    devices.airplay = {repaired};
    Settings twice = s;
    SavedOutput other = pair;
    other.id = other.airplay.deviceId = "OTHER";
    twice.outputs.push_back(other);
    EXPECT_TRUE(syncOutputs(twice, devices).empty());
    EXPECT_NE(saved(twice, Backend::AirPlay, "OLD-PAIR"), nullptr);

    EXPECT_EQ(syncOutputs(s, devices), (std::vector<OutputMove>{{Backend::AirPlay, "OLD-PAIR", "NEW-PAIR"}}));
}

// No two enabled outputs of one backend show the same name, as the settings of 0.5.1 could have them; the name offered
// instead counts on.
TEST(OutputsTest, TwoEnabledOutputsOfOneNameAreFoundAndOfferedANumberedName) {
    Settings s = fresh();
    s.outputs.push_back(savedPlayer("uuid:a", "DMP-A6", "Eversolo DMP-A6", ""));
    s.outputs.push_back(savedPlayer("uuid:b", "DMP-A6", "Eversolo DMP-A6", ""));
    const auto rows = outputRows(s, catalog());
    const auto& a = row(rows, Backend::Network, "uuid:a");
    const auto& b = row(rows, Backend::Network, "uuid:b");
    EXPECT_TRUE(nameTaken(rows, b));
    EXPECT_EQ(freeName(rows, b), "DMP-A6 (2)");
    renameOutput(s, Backend::Network, "uuid:b", "DMP-A6 (2)", catalog());
    const auto renamed = outputRows(s, catalog());
    EXPECT_FALSE(nameTaken(renamed, row(renamed, Backend::Network, "uuid:b")));
    EXPECT_FALSE(nameTaken(renamed, a));
    // A third of that name is offered the next number.
    s.outputs.push_back(savedPlayer("uuid:c", "DMP-A6", "Eversolo DMP-A6", ""));
    const auto three = outputRows(s, catalog());
    EXPECT_EQ(freeName(three, row(three, Backend::Network, "uuid:c")), "DMP-A6 (3)");
    // One turned off takes no name.
    setOutputEnabled(s, Backend::Network, "uuid:c", false, catalog());
    const auto off = outputRows(s, catalog());
    EXPECT_FALSE(nameTaken(off, row(off, Backend::Network, "uuid:a")));
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

// Plays a queue of two tracks on capture devices and records which outputs the Server opens. Choosing another output
// selects its zone, every zone keeping its queue (MZ-R07).
class OutputSwitchTest : public ::testing::Test {
protected:
    OutputSwitchTest()
        : server_(start(), {}, [this](const Settings& s) {
              size_t number = 0;
              {
                  std::scoped_lock lock(mutex_);
                  opened_.push_back(s.backend);
                  number = opened_.size();
              }
              audio::CaptureOptions options;
              // A file of its own, as zones play at once.
              options.file = testutil::tempDir() / ("output-switch-" + std::to_string(number) + ".raw");
              options.realtime = true;
              return audio::createCaptureOutput(options);
          }) {
        playHere();
    }

    // The two tracks in the selected zone.
    void playHere() {
        const auto first = testutil::generatedFile({"output-switch-first", 44100, 16, 2, 6.0});
        const auto second = testutil::generatedFile({"output-switch-second", 44100, 16, 2, 1.0});
        Command replace;
        replace.kind = CommandKind::QueueReplace;
        replace.sources = {TrackSource(toUtf8(first.wstring())), TrackSource(toUtf8(second.wstring()))};
        server_.submit(std::move(replace));
    }

    static const ZoneSnapshot& zoneOf(const Snapshot& s, const std::string& id) {
        const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
        if (found == s.zones.end()) throw std::runtime_error("no zone " + id);
        return *found;
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

TEST_F(OutputSwitchTest, SelectingAnotherOutputLeavesThePlayingZoneAsItWas) {
    waitUntilPlaying();
    const Snapshot before = server_.snapshot();
    const std::string first = before.selectedZone;
    switchTo(Backend::Asio, "a-dave");
    const Snapshot after = server_.snapshot();
    EXPECT_EQ(after.selectedZone, zoneId(Backend::Asio, "a-dave"));
    EXPECT_TRUE(after.queue.entries.empty());
    const auto& left = zoneOf(after, first);
    EXPECT_EQ(left.playback.state, PlaybackState::Playing);
    EXPECT_EQ(left.queue.entries, before.queue.entries);
    EXPECT_EQ(left.queueEntryId, before.queueEntryId);
    EXPECT_GE(left.playback.position + 4410, before.playback.position);
    // Played in the zone selected, the tracks open its output.
    playHere();
    ASSERT_TRUE(waitFor([](const Snapshot& s) {
        return s.playback.state == PlaybackState::Playing && s.playback.activeBackend == Backend::Asio;
    }));
    EXPECT_EQ(opened().back(), Backend::Asio);
    EXPECT_FALSE(server_.snapshot().playback.error) << server_.snapshot().playback.errorDetail;
}

TEST_F(OutputSwitchTest, TheZoneLeftGoesOnToItsNextTrack) {
    waitUntilPlaying();
    const auto before = server_.snapshot();
    ASSERT_EQ(before.queue.entries.size(), 2U);
    switchTo(Backend::Ks, "k-dave");
    Command seek;
    seek.kind = CommandKind::Seek;
    seek.zone = before.selectedZone;
    seek.frame = 5 * kRate;
    server_.submit(std::move(seek));
    const uint64_t second = before.queue.entries[1].id;
    EXPECT_TRUE(waitFor([&](const Snapshot& s) { return zoneOf(s, before.selectedZone).queueEntryId == second; }));
}

TEST_F(OutputSwitchTest, APausedZoneStaysPausedWhileAnotherIsSelected) {
    waitUntilPlaying();
    Command pause;
    pause.kind = CommandKind::Pause;
    server_.submit(std::move(pause));
    ASSERT_TRUE(waitFor([](const Snapshot& s) { return s.playback.state == PlaybackState::Paused; }));
    const auto before = server_.snapshot();
    switchTo(Backend::Asio, "a-dave");
    const auto left = zoneOf(server_.snapshot(), before.selectedZone);
    EXPECT_EQ(left.playback.state, PlaybackState::Paused);
    EXPECT_NEAR(double(left.playback.position), double(before.playback.position), 4410.0);
    EXPECT_EQ(server_.snapshot().settings.backend, Backend::Asio);

    switchTo(Backend::Wasapi, "");
    EXPECT_EQ(server_.snapshot().selectedZone, before.selectedZone);
    Command play;
    play.kind = CommandKind::Play;
    server_.submit(std::move(play));
    EXPECT_TRUE(waitFor([](const Snapshot& s) {
        return s.playback.state == PlaybackState::Playing && s.playback.activeBackend == Backend::Wasapi;
    }));
}

TEST_F(OutputSwitchTest, BetweenTwoDevicesOfOneBackendOpensTheNewDevice) {
    waitUntilPlaying();
    const size_t opens = opened().size();
    switchTo(Backend::Wasapi, "w-dave");
    playHere();
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

// The output menu lists only enabled rows, so a receiver turned off there could never be taken out of the group.
TEST(AirPlayGroupTest, AReceiverTurnedOffLeavesTheGroup) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    setOutputEnabled(s, Backend::AirPlay, "HP-B", false, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    EXPECT_FALSE(saved(s, Backend::AirPlay, "HP-B")->enabled);
}

TEST(AirPlayGroupTest, AReceiverThatBecameUnsupportedLeavesTheGroup) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    setInAirPlayGroup(s, "HP-B", true, withReceivers());
    auto devices = withReceivers();
    devices.airplay[1].unsupported = "refused";
    syncOutputs(s, devices);
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    EXPECT_FALSE(saved(s, Backend::AirPlay, "HP-B")->enabled);
}

// Zone groups (MZ-R24): a zone not in use takes the others and is left alone again, each keeping its own entry.
TEST(AirPlayGroupTest, AZoneNotInUseIsGroupedWithOthersAndUngrouped) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", withReceivers());
    setOutputEnabled(s, Backend::AirPlay, "HP-A", true, withReceivers());
    EXPECT_TRUE(groupAirPlay(s, "HP-A", {"HP-B", "PAIR", "HP-B"}, withReceivers()).empty());
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-A")->airplayGroup, (std::vector<std::string>{"HP-A", "HP-B", "PAIR"}));
    EXPECT_EQ(withOutputParameters(s, Backend::AirPlay, "HP-A", withReceivers()).airplayGroup,
              (std::vector<std::string>{"HP-A", "HP-B", "PAIR"}));
    EXPECT_TRUE(saved(s, Backend::AirPlay, "HP-B")->enabled);
    EXPECT_TRUE(saved(s, Backend::AirPlay, "PAIR")->enabled);
    EXPECT_EQ(s.backend, Backend::Asio);
    groupAirPlay(s, "HP-A", {"PAIR"}, withReceivers());
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-A")->airplayGroup, (std::vector<std::string>{"HP-A", "PAIR"}));
    groupAirPlay(s, "HP-A", {}, withReceivers());
    EXPECT_TRUE(saved(s, Backend::AirPlay, "HP-A")->airplayGroup.empty());
    EXPECT_TRUE(saved(s, Backend::AirPlay, "HP-B")->enabled);
    EXPECT_TRUE(saved(s, Backend::AirPlay, "PAIR")->enabled);
}

// A receiver of another group moves over; the receiver another group was formed with leaves it to the next.
TEST(AirPlayGroupTest, AReceiverOfAnotherGroupMovesOverAndItsGroupGoesOnUnderTheNext) {
    Settings s = fresh();
    selectOutput(s, Backend::Asio, "a-dave", withReceivers());
    groupAirPlay(s, "HP-B", {"PAIR"}, withReceivers());
    groupAirPlay(s, "HP-A", {"PAIR"}, withReceivers());
    EXPECT_TRUE(saved(s, Backend::AirPlay, "HP-B")->airplayGroup.empty());
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-A")->airplayGroup, (std::vector<std::string>{"HP-A", "PAIR"}));

    groupAirPlay(s, "HP-A", {}, withReceivers());
    groupAirPlay(s, "HP-B", {"PAIR"}, withReceivers());
    std::ranges::find_if(s.outputs, [](const SavedOutput& o) { return o.id == "HP-B"; })->volumeDb = -21.0;
    const auto moves = groupAirPlay(s, "HP-A", {"HP-B"}, withReceivers());
    ASSERT_EQ(moves.size(), 1U);
    EXPECT_EQ(moves[0].from, "HP-B");
    EXPECT_EQ(moves[0].to, "PAIR");
    EXPECT_EQ(saved(s, Backend::AirPlay, "PAIR")->volumeDb, -21.0);
    EXPECT_TRUE(saved(s, Backend::AirPlay, "PAIR")->airplayGroup.empty());
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-A")->airplayGroup, (std::vector<std::string>{"HP-A", "HP-B"}));
}

// The zone in use edits its own group; joining another, alone, it makes that one in use, and with a group of its own it
// leaves that group to the next, which stays in use.
TEST(AirPlayGroupTest, TheZoneInUseGroupsAndJoinsAnother) {
    Settings s = fresh();
    selectOutput(s, Backend::AirPlay, "HP-A", withReceivers());
    groupAirPlay(s, "HP-A", {"HP-B"}, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A", "HP-B"}));
    groupAirPlay(s, "HP-A", {}, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));

    groupAirPlay(s, "HP-B", {"HP-A"}, withReceivers());
    EXPECT_EQ(s.airplay.deviceId, "HP-B");
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-B", "HP-A"}));

    selectOutput(s, Backend::AirPlay, "PAIR", withReceivers());
    groupAirPlay(s, "PAIR", {"HP-A"}, withReceivers());
    EXPECT_EQ(group(s), (std::vector<std::string>{"PAIR", "HP-A"}));
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-B")->airplayGroup, (std::vector<std::string>{}));
    groupAirPlay(s, "HP-B", {"PAIR"}, withReceivers());
    EXPECT_EQ(s.airplay.deviceId, "HP-A");
    EXPECT_EQ(group(s), (std::vector<std::string>{"HP-A"}));
    EXPECT_EQ(saved(s, Backend::AirPlay, "HP-B")->airplayGroup, (std::vector<std::string>{"HP-B", "PAIR"}));
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

// A device scan while an output's settings dialog is open: its outputs reach the settings the window keeps at once,
// so closing it neither shows the outputs from before nor writes them back over the Server's; the page is filled once
// the dialog closes.
TEST(OutputsFollowTest, OutputsChangedWhileAnOutputsDialogIsOpenReachTheSettings) {
    SettingsDto settings, server;
    server.outputs.emplace_back();
    server.outputs.back().id = "new-dac";
    server.followedDefault = "new-dac";
    EXPECT_EQ(ui::followServerOutputs(settings, server, true), ui::OutputsRefill::None);
    ASSERT_EQ(settings.outputs.size(), 1u);
    EXPECT_EQ(settings.outputs.front().id, "new-dac");
    EXPECT_EQ(settings.followedDefault, "new-dac");
    EXPECT_EQ(ui::followServerOutputs(settings, server, false), ui::OutputsRefill::Outputs);
}

// One name that is not UTF-8, kept from a device, stopped every later save of the settings.
TEST(OutputsTest, ANameThatIsNotUtf8DoesNotStopTheSettingsBeingSaved) {
    Settings s;
    s.outputs.emplace_back();
    s.outputs.back().name = "Caf\xE9";
    std::string text;
    EXPECT_NO_THROW(text = toJson(s));
    EXPECT_NE(text.find("Caf"), std::string::npos);
}

namespace {

OutputDsp someDsp() {
    OutputDsp dsp;
    dsp.eq.enabled = true;
    dsp.eq.bands[2].gainDb = -6.0;
    dsp.eq.preset = "Warm";
    dsp.crossfeed.enabled = true;
    dsp.crossfeed.preset = CrossfeedPreset::JanMeier;
    dsp.convolution = {true, "C:\\Rooms\\desk.wav"};
    dsp.dspOrder = {audio::DspNode::Convolution, audio::DspNode::Eq, audio::DspNode::Crossfeed};
    dsp.replayGain.mode = ReplayGainMode::Album;
    dsp.replayGain.preampDb = 3.0;
    dsp.replayGain.preventClipping = false;
    return dsp;
}

} // namespace

TEST(OutputDspTest, EachOutputKeepsItsOwnAndAChangeReachesOnlyTheOneInUse) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    setOutputDsp(s, someDsp());
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(outputDsp(s), OutputDsp{});
    s.crossfeed.enabled = true;
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_EQ(outputDsp(s), someDsp());
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_TRUE(s.crossfeed.enabled);
    EXPECT_FALSE(s.eq.enabled);
    EXPECT_EQ(s.replayGain.mode, ReplayGainMode::Off);
}

TEST(OutputDspTest, AnOutputNewToItStartsWithAllOfItOff) {
    Settings s = fresh();
    setOutputDsp(s, someDsp());
    syncOutputs(s, catalog());
    setOutputEnabled(s, Backend::Ks, "k-dave", true, catalog());
    selectOutput(s, Backend::Ks, "k-dave", catalog());
    EXPECT_EQ(outputDsp(s), OutputDsp{});
    selectOutput(s, Backend::Wasapi, "", catalog());
    EXPECT_EQ(outputDsp(s), someDsp());
}

TEST(OutputDspTest, EveryOutputSharesTheEqPresets) {
    Settings s = fresh();
    s.eq.presets = {{"Warm", defaultEqBands()}};
    s.eq.preset = "Warm";
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(s.eq.presets.size(), 1U);
    EXPECT_EQ(s.eq.preset, "");
    s.eq.presets.push_back({"Flat", defaultEqBands()});
    syncOutputs(s, catalog());

    selectOutput(s, Backend::Wasapi, "", catalog());
    EXPECT_EQ(s.eq.presets.size(), 2U);
    EXPECT_EQ(s.eq.preset, "Warm");
    for (const auto& output : s.outputs) EXPECT_TRUE(output.dsp.eq.presets.empty()) << output.id;
}

TEST(OutputDspTest, EachOutputsSurvivesTheFile) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    setOutputDsp(s, someDsp());
    syncOutputs(s, catalog());
    selectOutput(s, Backend::Asio, "a-dave", catalog());
    s.replayGain.mode = ReplayGainMode::Track;
    syncOutputs(s, catalog());

    const Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.outputs, s.outputs);
    EXPECT_EQ(saved(read, Backend::Wasapi, "w-dave")->dsp, someDsp());
    EXPECT_EQ(saved(read, Backend::Asio, "a-dave")->dsp.replayGain.mode, ReplayGainMode::Track);
}

// settings.json as 0.4.2 wrote it, with one DSP for every output.
TEST(OutputDspTest, AnUpgradeGivesTheOneDspToEverySavedOutput) {
    Settings s =
        fromJson(R"({"schema":4,"backend":"asio","eq":{"enabled":true,"bands":[{"type":"peaking","frequency":1000,"gain":-6}],)"
                 R"("presets":[{"name":"Warm","bands":[]}]},"crossfeed":{"enabled":true,"preset":"janMeier"},)"
                 R"("replayGain":{"mode":"album"},"asio":{"driver":"a-dave"},"outputs":[)"
                 R"({"backend":"asio","id":"a-dave","enabled":true},{"backend":"wasapi","id":"w-dave","enabled":true}]})");
    ASSERT_EQ(s.outputs.size(), 2U);
    for (const auto& output : s.outputs) {
        EXPECT_TRUE(output.dsp.eq.enabled) << output.id;
        EXPECT_EQ(output.dsp.eq.bands.size(), 1U) << output.id;
        EXPECT_EQ(output.dsp.crossfeed.preset, CrossfeedPreset::JanMeier) << output.id;
        EXPECT_EQ(output.dsp.replayGain.mode, ReplayGainMode::Album) << output.id;
        EXPECT_TRUE(output.dsp.eq.presets.empty()) << output.id;
    }
    syncOutputs(s, catalog());
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    EXPECT_TRUE(s.eq.enabled);
    EXPECT_EQ(s.eq.presets.size(), 1U);
    selectOutput(s, Backend::Ks, "k-dave", catalog());
    EXPECT_EQ(outputDsp(s), OutputDsp{});
}

TEST(OutputDspTest, AnOutputsDialogTakesAnotherOutputsDsp) {
    Settings s = fresh();
    selectOutput(s, Backend::Wasapi, "w-dave", catalog());
    setOutputDsp(s, someDsp());
    syncOutputs(s, catalog());
    selectOutput(s, Backend::Asio, "a-dave", catalog());

    Settings edited = withOutputParameters(s, Backend::Ks, "k-dave", catalog());
    setOutputDsp(edited, outputDsp(withOutputParameters(s, Backend::Wasapi, "w-dave", catalog())));
    keepOutputParameters(s, edited, Backend::Ks, "k-dave", catalog());
    EXPECT_EQ(outputDsp(s), OutputDsp{});
    EXPECT_EQ(saved(s, Backend::Ks, "k-dave")->dsp, someDsp());

    Settings inUse = withOutputParameters(s, Backend::Asio, "a-dave", catalog());
    setOutputDsp(inUse, outputDsp(withOutputParameters(s, Backend::Ks, "k-dave", catalog())));
    keepOutputParameters(s, inUse, Backend::Asio, "a-dave", catalog());
    EXPECT_EQ(outputDsp(s), someDsp());
}

// The Windows default moving to another device while it is followed brings that device's DSP to the window, which would
// otherwise write the one it shows over it at its next change.
TEST(OutputsFollowTest, TheDspOfTheDefaultDeviceMovedToReachesTheWindow) {
    Settings server = fresh();
    setOutputDsp(server, someDsp());
    server.eq.presets = {{"Warm", defaultEqBands()}};
    syncOutputs(server, catalog());
    SettingsDto window = settingsDto(server);
    DeviceCatalog moved = catalog();
    moved.defaultWasapi = "w-dave";
    moved.revision = 2;
    syncOutputs(server, moved);
    window.eq.presets.push_back({"Flat", defaultEqBands()});

    EXPECT_EQ(ui::followServerOutputs(window, settingsDto(server), false), ui::OutputsRefill::Settings);
    Settings shown = nativeSettings(window);
    EXPECT_EQ(outputDsp(shown), OutputDsp{});
    EXPECT_EQ(shown.eq.presets.size(), 2U);

    SettingsDto unmoved = settingsDto(server);
    unmoved.eq.enabled = true; // a change the window made that the Server has not taken yet
    EXPECT_EQ(ui::followServerOutputs(unmoved, settingsDto(server), false), ui::OutputsRefill::Outputs);
    EXPECT_TRUE(unmoved.eq.enabled);
}

TEST_F(OutputSwitchTest, TheNewOutputPlaysWithItsOwnDsp) {
    waitUntilPlaying();
    const auto processed = [](const Snapshot& s) {
        return s.playback.state == PlaybackState::Playing && s.playback.plan &&
               s.playback.plan->state == audio::PipelineState::Processed;
    };
    Settings s = nativeSettings(server_.snapshot().settings);
    s.eq.enabled = true;
    s.eq.bands[2].gainDb = -6.0;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(s);
    server_.submit(std::move(apply));
    ASSERT_TRUE(waitFor(processed));

    const std::string first = server_.snapshot().selectedZone;
    switchTo(Backend::Wasapi, "w-dave");
    playHere();
    EXPECT_TRUE(waitFor([&](const Snapshot& n) {
        return n.settings.wasapi.deviceId == "w-dave" && n.playback.state == PlaybackState::Playing && n.playback.plan &&
               !processed(n);
    }));
    EXPECT_EQ(zoneOf(server_.snapshot(), first).playback.state, PlaybackState::Playing);
    switchTo(Backend::Wasapi, "");
    EXPECT_TRUE(waitFor(processed));
}
