// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// JSON form of the Server API (docs/Extension-points.md #4): every field survives a round trip through text, enums are
// stable string IDs, and malformed input is refused.
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "Server/Api/ApiJson.h"
#include "Server/Playback/LocalServer.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using nlohmann::json;

namespace {

template <class T, class F> T throughText(const T& value, F decode) {
    return decode(json::parse(toJson(value).dump()));
}

Command fullCommand() {
    Command c;
    c.kind = CommandKind::QueueReplace;
    c.source = TrackSource("C:/music/a.flac", {10, 2000}, DecoderKind::DrFlac);
    SettingsDto settings;
    settings.backend = Backend::Asio;
    settings.decoderByFormat[AudioFormat::Mp3] = DecoderKind::DrMp3;
    settings.pcmBitsByFormat[AudioFormat::Aac] = 16;
    settings.convertWhenUnsupported = true;
    settings.wasapi.fixedRate = 96000;
    settings.ks.mode = KsMode::Standard;
    settings.language = "zh-TW";
    settings.lastFileUtf8 = "C:/music/曲.flac";
    c.settings = settings;
    c.frame = 123456;
    c.sources = {TrackSource("C:/music/b.wav"), TrackSource("C:/music/c.m4a", {0, std::nullopt}, DecoderKind::Symphonia)};
    c.index = 1;
    c.entryIds = {3, 1, 2};
    c.entryId = 7;
    c.rootId = 42;
    c.shuffle = true;
    c.repeat = RepeatMode::One;
    return c;
}

Snapshot fullSnapshot() {
    Snapshot s;
    s.revision = 41;
    s.command = {9, 2, 5};
    s.queueEntryId = 3;
    s.track = TrackSource("C:/music/a.dsf", {}, DecoderKind::DsdNexus);
    s.queue.revision = 5;
    s.queue.entries = {{3, TrackSource("C:/music/a.dsf")}, {4, TrackSource("C:/music/b.flac", {5, 6})}};
    s.queue.upcomingEntryId = 4;
    s.queue.shuffle = true;
    s.queue.repeat = RepeatMode::All;
    auto& p = s.playback;
    p.state = PlaybackState::Paused;
    p.pathUtf8 = "C:/music/a.dsf";
    p.position = 1000;
    p.total = 200000;
    p.source = SourceInfo{352800, 2, 1, 200000, std::array<uint8_t, 16>{0x00, 0x01, 0xab, 0xff}};
    p.format = AudioFormat::Dsd;
    p.output = DeviceFormat{176400, 2, SampleType::Int24In32};
    OutputPlan plan{{176400, 2, SampleType::Int24In32}};
    plan.duplicate = plan.downmix = plan.resample = plan.widen = plan.reduce = plan.shared = true;
    plan.dsd = DsdPath::Dop;
    plan.state = PipelineState::LossySource;
    p.plan = plan;
    p.backend = Backend::Ks;
    p.decoder = DecoderKind::DsdNexus;
    p.activeBackend = Backend::Ks;
    p.activeDecoder = DecoderKind::DsdNexus;
    p.outputMode = "KS · WaveRT event";
    p.outputDetail = "period 10 ms";
    p.outputAdjustments = {"Fixed sample rate: requested 96000 Hz, using 44100 Hz"};
    p.bufferFrames = 512;
    p.underruns = 2;
    p.error = ErrorCode::DeviceLost;
    p.errorDetail = "unplugged";
    p.revision = 17;
    s.devices = {8, {{"w1", "Speakers"}}, {{"{CLSID}", "Chord ASIO"}}, {{"k|1", "DAVE"}, {"k|2", "Cirrus"}}};
    s.commandError = Failure{ErrorCode::FormatNotSupported, "no bit-perfect format"};
    s.controlPanelOpened = false;
    s.settings.backend = Backend::Wasapi;
    s.settings.wasapi.shared = true;
    return s;
}

} // namespace

TEST(ApiJson, CommandSurvivesTheRoundTripWithStringIds) {
    const Command c = fullCommand();
    EXPECT_EQ(throughText(c, commandFromJson), c);
    EXPECT_EQ(throughText(Command{}, commandFromJson), Command{});
    const json j = toJson(c);
    EXPECT_EQ(j["kind"], "queueReplace");
    EXPECT_EQ(j["source"]["decoder"], "drflac");
    EXPECT_EQ(j["repeat"], "one");
    EXPECT_EQ(j["settings"]["backend"], "asio");
    EXPECT_TRUE(j["sources"][0]["decoder"].is_null());
    EXPECT_TRUE(j["sources"][1]["end"].is_null());
}

TEST(ApiJson, SnapshotAndEventSurviveTheRoundTrip) {
    const Snapshot s = fullSnapshot();
    EXPECT_EQ(throughText(s, snapshotFromJson), s);
    EXPECT_EQ(throughText(Snapshot{}, snapshotFromJson), Snapshot{});
    const Event e{EventKind::CommandCompleted, s};
    EXPECT_EQ(throughText(e, eventFromJson), e);
    const json j = toJson(s);
    EXPECT_EQ(j["playback"]["source"]["md5"], "0001abff000000000000000000000000");
    EXPECT_EQ(j["playback"]["plan"]["dsd"], "dop");
    EXPECT_EQ(j["playback"]["output"]["type"], "24in32");
    EXPECT_EQ(j["playback"]["error"], "deviceLost");
    EXPECT_EQ(j["playback"]["format"], "dsd");
    EXPECT_EQ(throughText(CommandStamp{1, 2, 3}, commandStampFromJson), (CommandStamp{1, 2, 3}));
}

TEST(ApiJson, LiveLocalServerSnapshotSurvivesTheRoundTrip) {
    LocalServer server(Settings{}, std::filesystem::path{});
    server.waitIdle();
    const Snapshot s = server.snapshot();
    EXPECT_EQ(throughText(s, snapshotFromJson), s);
}

TEST(ApiJson, RootRemovalRoundTripsAndRequiresAnIntegerRootId) {
    Command c;
    c.kind = CommandKind::RemoveLibraryRoot;
    c.rootId = 42;
    EXPECT_EQ(throughText(c, commandFromJson), c);
    auto j = toJson(c);
    j.erase("rootId");
    EXPECT_THROW(commandFromJson(j), std::invalid_argument);
    j["rootId"] = 1.5;
    EXPECT_THROW(commandFromJson(j), std::invalid_argument);
    c.kind = CommandKind::Stop;
    c.rootId = 0;
    j = toJson(c);
    j.erase("rootId");
    EXPECT_EQ(commandFromJson(j), c);
}

TEST(ApiJson, PlayStatisticsResetRoundTripsTrackKeys) {
    Command c;
    c.kind = CommandKind::ResetPlayStatistics;
    c.tracks = {{7, 0}, {8, 3}};
    EXPECT_EQ(throughText(c, commandFromJson), c);
    auto j = toJson(c);
    j.erase("tracks");
    EXPECT_THROW(commandFromJson(j), std::invalid_argument);
    j["tracks"] = json::array({{{"file", 7}}});
    EXPECT_THROW(commandFromJson(j), std::invalid_argument);
    Command all;
    all.kind = CommandKind::ResetAllPlayStatistics;
    EXPECT_EQ(throughText(all, commandFromJson), all);
    j = toJson(all);
    j.erase("tracks");
    EXPECT_EQ(commandFromJson(j), all);
}

TEST(ApiJson, MalformedInputIsRefused) {
    json j = toJson(fullCommand());
    for (const char* key : {"kind", "frame", "sources", "repeat"}) {
        json missing = j;
        missing.erase(key);
        EXPECT_THROW(commandFromJson(missing), std::invalid_argument) << key;
    }
    json bad = j;
    bad["kind"] = "rewind";
    EXPECT_THROW(commandFromJson(bad), std::invalid_argument);
    bad = j;
    bad["frame"] = -1;
    EXPECT_THROW(commandFromJson(bad), std::invalid_argument);
    bad = j;
    bad["source"]["decoder"] = "winamp";
    EXPECT_THROW(commandFromJson(bad), std::invalid_argument);
    bad = j;
    bad["shuffle"] = "yes";
    EXPECT_THROW(commandFromJson(bad), std::invalid_argument);
    json snapshot = toJson(fullSnapshot());
    snapshot["playback"]["source"]["md5"] = "xyz";
    EXPECT_THROW(snapshotFromJson(snapshot), std::invalid_argument);
    EXPECT_THROW(snapshotFromJson(json::array()), std::invalid_argument);
}
