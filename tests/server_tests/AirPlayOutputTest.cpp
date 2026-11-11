// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/util/Text.h"
#include "FakeAirPlayReceiver.h"
#include "Server/Network/AirPlayFinder.h"
#include "Server/Network/AirPlayOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;
using airplay::kFramesPerPacket;
using audio::DeviceFormat;
using audio::SampleType;
using testutil::FakeMdns;
using testutil::FakeReceiverRecord;
using testutil::FakeRtsp;
using testutil::pcmOfAlac;

namespace {

constexpr DeviceFormat kCd{44100, 2, SampleType::Int16};
constexpr uint64_t kSecond = 44100;

bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return check();
}

class CountingSource final : public audio::IRenderSource {
public:
    explicit CountingSource(uint64_t first, uint64_t second = 0) : first_(first), second_(second) {}

    size_t render(uint8_t* destination, size_t frames) noexcept override {
        boundary_ = kNoBoundary;
        const uint64_t at = at_.load();
        const size_t n = size_t(std::min<uint64_t>(frames, first_ + second_ - at));
        for (size_t i = 0; i < n * 4; ++i) destination[i] = byteAt(at * 4 + i);
        if (second_ && at < first_ && at + n >= first_) boundary_ = size_t(first_ - at);
        at_ += n;
        return n;
    }
    size_t boundaryInLastRender() const noexcept override { return boundary_; }
    bool renderEnded() const noexcept override { return at_.load() == first_ + second_; }
    audio::StreamTrack streamTrack() const override { return {L"first.flac", first_, 0, 1}; }
    std::optional<audio::StreamTrack> nextStreamTrack() const override {
        if (!second_) return std::nullopt;
        return audio::StreamTrack{L"second.flac", second_, 0, 2};
    }
    uint64_t rendered() const { return at_.load(); }

    static uint8_t byteAt(uint64_t at) { return uint8_t(at * 7 >> 3); }

private:
    uint64_t first_, second_;
    std::atomic<uint64_t> at_ = 0;
    size_t boundary_ = kNoBoundary;
};

TrackDetails sampleDetails(const std::wstring& file) {
    return {"Title", "Artist", "Album", file == L"first.flac" ? "jpeg bytes" : ""};
}

class AirPlayOutputTest : public ::testing::Test {
protected:
    airplay::Receiver receiver() const {
        return {"800A805D016B", "DMP-A6", "127.0.0.1", rtsp_.port(), "105.1", airplay::Support::Supported, others_};
    }

    std::unique_ptr<audio::IOutput> output(std::function<std::optional<double>()> volume = {}) {
        AirPlayOutputOptions options;
        options.details = sampleDetails;
        options.volumeDb = std::move(volume);
        options.requestTimeoutMs = 500;
        options.heartbeat = 50ms;
        options.closeSessionAfter = 300ms;
        auto created = createAirPlayOutput(receiver(), options);
        created->setErrorHandler([this](const audio::Error& e) { error_ = int(e.code()) + 1; });
        return created;
    }

    bool packets(size_t count) {
        return waitFor([&] { return rtsp_.packetCount() >= count; });
    }

    FakeRtsp rtsp_;
    std::vector<airplay::Speaker> others_;
    std::atomic<int> error_ = 0;
};

} // namespace

TEST_F(AirPlayOutputTest, TakesOnlyCdFormat) {
    auto out = output();
    EXPECT_TRUE(out->supports(kCd));
    EXPECT_FALSE(out->supports({48000, 2, SampleType::Int16}));
    EXPECT_FALSE(out->supports({44100, 2, SampleType::Int24}));
    EXPECT_FALSE(out->supports({44100, 1, SampleType::Int16}));
}

TEST_F(AirPlayOutputTest, FramesArriveBitForBitInOrder) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(40));
    out->close();

    const auto packets = rtsp_.packets();
    EXPECT_TRUE(packets.front().marker);
    for (size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].sequence, uint16_t(packets.front().sequence + i));
        EXPECT_EQ(packets[i].rtpTime, uint32_t(packets.front().rtpTime + i * kFramesPerPacket));
        if (i) EXPECT_FALSE(packets[i].marker);
        const std::string pcm = pcmOfAlac(packets[i].alac);
        ASSERT_EQ(pcm.size(), 1408U);
        for (size_t b = 0; b < pcm.size(); ++b) ASSERT_EQ(uint8_t(pcm[b]), CountingSource::byteAt(i * 1408 + b)) << i << " " << b;
    }
}

TEST_F(AirPlayOutputTest, TheSessionStartsUnencryptedInOrder) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    const auto methods = rtsp_.methods();
    EXPECT_EQ(methods, (std::vector<std::string>{"OPTIONS", "ANNOUNCE", "SETUP", "RECORD"}));
    for (const auto& request : rtsp_.requests()) {
        EXPECT_EQ(request.find("Apple-Challenge"), std::string::npos);
        EXPECT_EQ(request.find("rsaaeskey"), std::string::npos);
        EXPECT_EQ(request.find("fpaeskey"), std::string::npos);
    }
    EXPECT_NE(rtsp_.requestsOf("SETUP").front().find("timing_port="), std::string::npos);
    EXPECT_NE(rtsp_.requestsOf("RECORD").front().find("Session: 1\r\n"), std::string::npos);
    EXPECT_NE(rtsp_.requestsOf("RECORD").front().find("RTP-Info: seq="), std::string::npos);
    out->close();
}

TEST_F(AirPlayOutputTest, RequestsCarryTheUserAgentHomePodsAccept) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    for (const auto& request : rtsp_.requests())
        EXPECT_NE(request.find("User-Agent: AirPlay/999.0.0 CamelPlayer\r\n"), std::string::npos) << request;
    out->close();
}

TEST_F(AirPlayOutputTest, EachSpeakerOfAPairGetsTheSameStream) {
    FakeRtsp second;
    others_ = {{"127.0.0.1", second.port()}};
    CountingSource source(kSecond * 4);
    double wanted = -12;
    auto out = output([&]() -> std::optional<double> { return wanted; });
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(40));
    ASSERT_TRUE(waitFor([&] { return second.packetCount() >= 40; }));
    out->pause();
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("FLUSH") == 1 && second.count("FLUSH") == 1; }));
    EXPECT_TRUE(waitFor([&] { return second.count("SET_PARAMETER") >= 1; }));
    EXPECT_EQ(rtsp_.requestsOf("SET_PARAMETER").front().find("volume: -12.000000") != std::string::npos,
              second.requestsOf("SET_PARAMETER").front().find("volume: -12.000000") != std::string::npos);
    out->close();
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("TEARDOWN") == 1 && second.count("TEARDOWN") == 1; }));

    const auto first = rtsp_.packets();
    const auto other = second.packets();
    const size_t count = std::min(first.size(), other.size());
    ASSERT_GE(count, 40U);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(first[i].sequence, other[i].sequence);
        EXPECT_EQ(first[i].rtpTime, other[i].rtpTime);
        EXPECT_EQ(first[i].alac, other[i].alac);
    }
    EXPECT_GE(second.controlPackets().size(), 1U);
}

TEST_F(AirPlayOutputTest, APairDoesNotOpenWhenASpeakerRefuses) {
    FakeRtsp second;
    second.answer("ANNOUNCE", 403);
    others_ = {{"127.0.0.1", second.port()}};
    CountingSource source(kSecond);
    auto out = output();
    EXPECT_THROW(out->open(kCd, &source), audio::Error);
    EXPECT_EQ(error_, 0);
}

TEST_F(AirPlayOutputTest, LosingEitherSpeakerOfAPairEndsTheStream) {
    FakeRtsp second;
    others_ = {{"127.0.0.1", second.port()}};
    CountingSource source(kSecond * 4);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return second.packetCount() >= 10; }));
    second.takeOver();
    EXPECT_TRUE(waitFor([&] { return error_ == int(audio::ErrorCode::DeviceInUse) + 1; }));
    out->close();
}

TEST_F(AirPlayOutputTest, ASyncPacketComesFirstAndEverySecond) {
    CountingSource source(kSecond * 3);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    EXPECT_TRUE(waitFor([&] { return rtsp_.controlPackets().size() >= 2; }));
    const auto syncs = rtsp_.controlPackets();
    ASSERT_EQ(syncs[0].size(), 20U);
    EXPECT_EQ(uint8_t(syncs[0][0]), 0x90);
    EXPECT_EQ(uint8_t(syncs[1][0]), 0x80);
    EXPECT_EQ(uint8_t(syncs[0][1]), 0xD4);
    const auto word = [](const std::string& p, size_t at) {
        return uint32_t(uint8_t(p[at])) << 24 | uint32_t(uint8_t(p[at + 1])) << 16 | uint32_t(uint8_t(p[at + 2])) << 8 |
               uint32_t(uint8_t(p[at + 3]));
    };
    EXPECT_EQ(word(syncs[0], 16) - word(syncs[0], 4), airplay::kSyncLatency);
    const uint32_t step = word(syncs[1], 16) - word(syncs[0], 16);
    EXPECT_NEAR(double(step), 44100.0, 8000.0);
    out->close();
}

TEST_F(AirPlayOutputTest, TimingIsAnsweredWhileTheSessionOpens) {
    rtsp_.timeBeforeSetup();
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    EXPECT_EQ(rtsp_.count("RECORD"), 1);
    out->close();
}

TEST_F(AirPlayOutputTest, TimingRequestsAreAnswered) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    std::string request("\x80\xD2\x00\x07", 4);
    request.resize(24, '\0');
    request += "ABCDEFGH";
    rtsp_.askTiming(request);
    ASSERT_TRUE(waitFor([&] { return !rtsp_.timingReplies().empty(); }));
    const auto reply = rtsp_.timingReplies().front();
    ASSERT_EQ(reply.size(), 32U);
    EXPECT_EQ(uint8_t(reply[1]), 0xD3);
    EXPECT_EQ(reply.substr(8, 8), "ABCDEFGH");
    out->close();
}

TEST_F(AirPlayOutputTest, AMissedPacketIsSentAgainFromHistory) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(10));
    const auto wanted = rtsp_.packets()[5];
    rtsp_.askResend(wanted.sequence, 1);
    std::string found;
    ASSERT_TRUE(waitFor([&] {
        for (const auto& packet : rtsp_.controlPackets())
            if (uint8_t(packet[1]) == 0xD6) found = packet;
        return !found.empty();
    }));
    EXPECT_EQ(uint16_t(uint8_t(found[2]) << 8 | uint8_t(found[3])), wanted.sequence);
    EXPECT_EQ(found.substr(16), wanted.alac);
    out->close();
}

TEST_F(AirPlayOutputTest, AResendIsSentOnlyToAReceiverOfTheSession) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(10));
    EXPECT_EQ(rtsp_.askResendFrom("127.0.0.2", rtsp_.packets()[5].sequence, 1, 300ms), 0U);
    out->close();
}

// A request for every sequence there is, again and again, does not hold the stream up.
TEST_F(AirPlayOutputTest, ResendRequestsForEverythingDoNotDelayTheStream) {
    CountingSource source(30 * kSecond);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(300));
    const size_t before = rtsp_.packetCount();
    const auto asked = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) rtsp_.askResend(0, 0xFFFF);
    ASSERT_TRUE(packets(before + 100));
    const auto arrived = rtsp_.packets();
    std::chrono::steady_clock::duration longest{};
    for (size_t i = before; i < arrived.size(); ++i)
        if (arrived[i].arrived >= asked) longest = std::max(longest, arrived[i].arrived - arrived[i - 1].arrived);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(longest).count(), 40);
    out->close();
}

TEST_F(AirPlayOutputTest, HeldFramesAreWhatTheReceiverHasNotPlayedYet) {
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(50));
    EXPECT_GT(out->status().heldFrames, 10000U);
    EXPECT_EQ(out->status().mode, "AirPlay · DMP-A6");
    out->close();
}

TEST_F(AirPlayOutputTest, WhatWasRenderedDrainsAfterTheLatency) {
    CountingSource source(4410);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return source.renderEnded(); }));
    EXPECT_GT(out->status().heldFrames, 0U);
    EXPECT_TRUE(waitFor([&] { return out->status().heldFrames == 0; }, 4s));
    out->close();
}

TEST_F(AirPlayOutputTest, TheLastPartOfATrackIsPaddedWithSilence) {
    CountingSource source(kFramesPerPacket + 100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(2));
    const auto pcm = pcmOfAlac(rtsp_.packets()[1].alac);
    ASSERT_EQ(pcm.size(), 1408U);
    for (size_t b = 0; b < 400; ++b) EXPECT_EQ(uint8_t(pcm[b]), CountingSource::byteAt(1408 + b));
    for (size_t b = 400; b < pcm.size(); ++b) EXPECT_EQ(pcm[b], '\0');
    out->close();
}

TEST_F(AirPlayOutputTest, APauseFlushesAndAResumeSendsWhatWasNotHeard) {
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(40));
    const auto held = out->status().heldFrames;
    EXPECT_GT(held, 0U);
    out->pause();
    ASSERT_EQ(rtsp_.count("FLUSH"), 1);
    const size_t sent = rtsp_.packetCount();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(rtsp_.packetCount(), sent);

    out->start();
    ASSERT_TRUE(packets(sent + 1));
    const auto packets = rtsp_.packets();
    EXPECT_TRUE(packets[sent].marker);
    EXPECT_EQ(packets[sent].sequence, uint16_t(packets[sent - 1].sequence + 1));
    EXPECT_EQ(packets[sent].rtpTime, uint32_t(packets[sent - 1].rtpTime + kFramesPerPacket));
    const auto pcm = pcmOfAlac(packets[sent].alac);
    ASSERT_EQ(pcm.size(), 1408U);
    EXPECT_EQ(uint8_t(pcm[10]), CountingSource::byteAt(10)) << "nothing was heard yet, so it starts again from the start";
    EXPECT_EQ(rtsp_.count("SETUP"), 1);
    out->close();
}

TEST_F(AirPlayOutputTest, ASeekGoesOnInTheSameSession) {
    CountingSource first(kSecond * 10);
    CountingSource second(kSecond * 10);
    auto out = output();
    out->open(kCd, &first);
    out->start();
    ASSERT_TRUE(packets(10));
    out->close();
    EXPECT_EQ(rtsp_.count("FLUSH"), 1);
    const size_t sent = rtsp_.packetCount();
    out->open(kCd, &second);
    out->start();
    ASSERT_TRUE(packets(sent + 1));
    EXPECT_EQ(rtsp_.count("SETUP"), 1);
    EXPECT_TRUE(rtsp_.packets()[sent].marker);
    out->close();
}

TEST_F(AirPlayOutputTest, TheSessionClosesWhenNothingFollows) {
    CountingSource source(44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(5));
    out->close();
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("TEARDOWN") == 1; }));

    CountingSource again(44100);
    out->open(kCd, &again);
    EXPECT_EQ(rtsp_.count("SETUP"), 2);
    out->close();
}

TEST_F(AirPlayOutputTest, TheReceiversVolumeIsSentAtTheStartAndWhenItChanges) {
    std::atomic<double> db = -20.1;
    CountingSource source(44100);
    auto out = output([&]() -> std::optional<double> { return db.load(); });
    out->open(kCd, &source);
    const auto hasVolume = [&](const char* text) {
        return waitFor([&] {
            return std::ranges::any_of(rtsp_.requestsOf("SET_PARAMETER"),
                                       [&](const std::string& r) { return r.find(text) != std::string::npos; });
        });
    };
    EXPECT_TRUE(hasVolume("volume: -20.100000\r\n"));
    db = airplay::kMutedDb;
    EXPECT_TRUE(hasVolume("volume: -144.000000\r\n"));
    out->close();
}

TEST_F(AirPlayOutputTest, AFixedVolumeSendsNoLevel) {
    CountingSource source(44100);
    auto out = output([]() -> std::optional<double> { return std::nullopt; });
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(10));
    for (const auto& request : rtsp_.requestsOf("SET_PARAMETER")) EXPECT_EQ(request.find("volume"), std::string::npos);
    out->close();
}

TEST_F(AirPlayOutputTest, EachTrackSendsItsTagsCoverAndProgressAtItsOwnRtpTime) {
    CountingSource source(8000, 44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    const auto tags = [&] {
        std::vector<std::string> found;
        for (const auto& request : rtsp_.requestsOf("SET_PARAMETER"))
            if (request.find("application/x-dmap-tagged") != std::string::npos) found.push_back(request);
        return found;
    };
    ASSERT_TRUE(waitFor([&] { return tags().size() == 2; }));
    EXPECT_TRUE(waitFor([&] { return rtsp_.packetCount() > 30; }));
    const auto first = rtsp_.packets().front().rtpTime;
    const auto sent = tags();
    EXPECT_NE(sent[0].find("Title"), std::string::npos);
    EXPECT_NE(sent[0].find("RTP-Info: rtptime=" + std::to_string(first) + "\r\n"), std::string::npos);
    EXPECT_NE(sent[1].find("RTP-Info: rtptime=" + std::to_string(first + 8000) + "\r\n"), std::string::npos);
    const auto all = rtsp_.requestsOf("SET_PARAMETER");
    EXPECT_EQ(std::ranges::count_if(all, [](const std::string& r) { return r.find("image/jpeg") != std::string::npos; }), 1);
    EXPECT_TRUE(std::ranges::any_of(all, [&](const std::string& r) {
        return r.find("progress: " + std::to_string(first) + "/" + std::to_string(first) + "/" + std::to_string(first + 8000)) !=
               std::string::npos;
    }));
    out->close();
}

TEST_F(AirPlayOutputTest, FramesRunStraightAcrossATrackChange) {
    CountingSource source(8000, 44100);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(40));
    const auto packets = rtsp_.packets();
    for (size_t i = 0; i < 40; ++i) {
        const std::string pcm = pcmOfAlac(packets[i].alac);
        ASSERT_EQ(pcm.size(), 1408U);
        for (size_t b = 0; b < pcm.size(); ++b) ASSERT_EQ(uint8_t(pcm[b]), CountingSource::byteAt(i * 1408 + b));
    }
    out->close();
}

TEST_F(AirPlayOutputTest, AnotherSenderTakingTheReceiverIsInUse) {
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(5));
    rtsp_.takeOver();
    EXPECT_TRUE(waitFor([&] { return error_ == int(audio::ErrorCode::DeviceInUse) + 1; }));
    out->close();
}

TEST_F(AirPlayOutputTest, AReceiverThatStopsAnsweringIsNotConnected) {
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(packets(5));
    rtsp_.stopAnswering();
    EXPECT_TRUE(waitFor([&] { return error_ == int(audio::ErrorCode::DeviceLost) + 1; }));
    out->close();
}

TEST_F(AirPlayOutputTest, ARefusalIsNotAFormatTheReceiverCouldTake) {
    rtsp_.answer("ANNOUNCE", 403);
    CountingSource source(44100);
    auto out = output();
    try {
        out->open(kCd, &source);
        FAIL() << "opened";
    } catch (const audio::Error& e) {
        EXPECT_EQ(e.code(), audio::ErrorCode::FormatNotSupported);
    }
}

TEST_F(AirPlayOutputTest, ABusyReceiverIsInUse) {
    rtsp_.answer("SETUP", 453);
    CountingSource source(44100);
    auto out = output();
    try {
        out->open(kCd, &source);
        FAIL() << "opened";
    } catch (const audio::Error& e) {
        EXPECT_EQ(e.code(), audio::ErrorCode::DeviceInUse);
    }
}

TEST(AirPlayPacketTest, TheSlidersLevelsBecomeTheReceiversScale) {
    EXPECT_DOUBLE_EQ(airplay::volumeDb(100, false), 0.0);
    EXPECT_DOUBLE_EQ(airplay::volumeDb(50, false), -15.0);
    EXPECT_NEAR(airplay::volumeDb(33, false), -20.1, 1e-9);
    EXPECT_DOUBLE_EQ(airplay::volumeDb(0, false), airplay::kMutedDb);
    EXPECT_DOUBLE_EQ(airplay::volumeDb(50, true), airplay::kMutedDb);
}

namespace {

// Plays through the whole Server to a fake on the network, found the way a real receiver is.
class AirPlayStreamingTest : public ::testing::Test {
protected:
    std::unique_ptr<LocalServer> serverFor(Settings settings = {}) {
        FakeReceiverRecord record;
        record.deviceId = "800A805D016B";
        record.name = "DMP-A6";
        record.host = "dmp";
        record.port = rtsp_.port();
        mdns_.add(record);
        settings.autoplay = false;
        auto server = std::make_unique<LocalServer>(settings);
        AirPlayFinderOptions options;
        options.queryHost = "127.0.0.1";
        options.queryPort = mdns_.port();
        options.searchInterval = 100ms;
        options.retryInterval = 50ms;
        server->setAirPlayFinderOptions(options);
        EXPECT_TRUE(waitFor([&] {
            Command again;
            again.kind = CommandKind::RefreshDevices;
            server->submit(std::move(again));
            server->waitIdle();
            return !server->snapshot().devices.airplay.empty();
        }));
        auto chosen = nativeSettings(server->snapshot().settings);
        selectOutput(chosen, Backend::AirPlay, "800A805D016B", server->snapshot().devices);
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(chosen);
        server->submit(std::move(apply));
        server->waitIdle();
        return server;
    }

    static Command queue(const std::vector<std::filesystem::path>& files) {
        Command replace;
        replace.kind = CommandKind::QueueReplace;
        for (const auto& file : files) replace.sources.emplace_back(toUtf8(file.wstring()));
        return replace;
    }

    std::string receivedPcm(size_t skipPackets = 0) const {
        std::string pcm;
        const auto packets = rtsp_.packets();
        for (size_t i = skipPackets; i < packets.size(); ++i) pcm += pcmOfAlac(packets[i].alac);
        return pcm;
    }

    FakeRtsp rtsp_;
    FakeMdns mdns_;
};

} // namespace

TEST_F(AirPlayStreamingTest, ATrackReachesTheReceiverBitForBit) {
    const auto file = testutil::generatedFile({"airplay-bitperfect", 44100, 16, 2, 0.5});
    const auto reference = audio::testaudio::referenceDecode(file);
    auto server = serverFor();
    server->submit(queue({file}));
    const size_t bytes = reference.samples.size() * 2;
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() * 1408 >= bytes; })) << server->snapshot().playback.errorDetail;
    const std::string pcm = receivedPcm();
    ASSERT_GE(pcm.size(), bytes);
    for (size_t i = 0; i < reference.samples.size(); ++i)
        ASSERT_EQ(int32_t(*reinterpret_cast<const int16_t*>(pcm.data() + i * 2)), reference.samples[i]) << i;
    EXPECT_TRUE(server->snapshot().playback.bitPerfect());
    EXPECT_EQ(server->snapshot().playback.activeBackend, Backend::AirPlay);
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, MatchingTracksRunTogetherWithoutAGap) {
    const auto first = testutil::generatedFile({"airplay-gapless-a", 44100, 16, 2, 0.6});
    const auto second = testutil::generatedFile({"airplay-gapless-b", 44100, 16, 2, 0.4});
    auto expected = audio::testaudio::referenceDecode(first).samples;
    const auto rest = audio::testaudio::referenceDecode(second).samples;
    expected.insert(expected.end(), rest.begin(), rest.end());
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() * 1408 >= expected.size() * 2; }))
        << server->snapshot().playback.errorDetail;
    EXPECT_EQ(rtsp_.count("FLUSH"), 0);
    const std::string pcm = receivedPcm();
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(int32_t(*reinterpret_cast<const int16_t*>(pcm.data() + i * 2)), expected[i]) << i;
    EXPECT_TRUE(waitFor([&] { return server->snapshot().queueEntryId == server->snapshot().queue.entries.back().id; }));
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, ThePositionShowsWhatIsHeardNotWhatWasSent) {
    const auto file = testutil::generatedFile({"airplay-position", 44100, 16, 2, 6.0});
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() > 100; })) << server->snapshot().playback.errorDetail;
    EXPECT_LT(server->snapshot().playback.position, 4410U);
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.position > 22050; }, 5s));
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, ASeekFlushesAndSendsTheNewPosition) {
    const auto file = testutil::generatedFile({"airplay-seek", 44100, 16, 2, 3.0});
    const auto reference = audio::testaudio::referenceDecode(file);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() > 20; })) << server->snapshot().playback.errorDetail;
    const size_t before = rtsp_.packetCount() + 200;
    Command seek;
    seek.kind = CommandKind::Seek;
    seek.frame = 88200;
    server->submit(std::move(seek));
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("FLUSH") == 1; }));
    ASSERT_TRUE(waitFor([&] {
        const auto packets = rtsp_.packets();
        return std::ranges::count_if(packets, &testutil::FakeRtp::marker) == 2 && packets.size() > before / 4;
    }));
    const auto packets = rtsp_.packets();
    const auto restart = std::find_if(packets.begin() + 1, packets.end(), [](const auto& p) { return p.marker; });
    ASSERT_NE(restart, packets.end());
    const std::string pcm = pcmOfAlac(restart->alac);
    ASSERT_EQ(pcm.size(), 1408U);
    for (size_t i = 0; i < 704; ++i)
        ASSERT_EQ(int32_t(*reinterpret_cast<const int16_t*>(pcm.data() + i * 2)), reference.samples[size_t{88200} * 2 + i]) << i;
    EXPECT_EQ(rtsp_.count("SETUP"), 1);
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, TheSlidersLevelIsKeptForEachReceiverAndSent) {
    const auto file = testutil::generatedFile({"airplay-volume", 44100, 16, 2, 2.0});
    auto server = serverFor();
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.playerVolume == 33U; }));
    server->submit(queue({file}));
    const auto sent = [&](const char* text) {
        return waitFor([&] {
            return std::ranges::any_of(rtsp_.requestsOf("SET_PARAMETER"),
                                       [&](const std::string& r) { return r.find(text) != std::string::npos; });
        });
    };
    EXPECT_TRUE(sent("volume: -20.100000"));
    Command level;
    level.kind = CommandKind::SetPlayerVolume;
    level.playerVolume = 50;
    server->submit(std::move(level));
    EXPECT_TRUE(sent("volume: -15.000000"));
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.playerVolume == 50U; }));
    Command mute;
    mute.kind = CommandKind::SetPlayerMute;
    mute.playerMute = true;
    server->submit(std::move(mute));
    EXPECT_TRUE(sent("volume: -144.000000"));
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.playerMuted; }));
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, ACrossfadeRunsInTheOneStream) {
    const auto first = testutil::generatedFile({"airplay-fade-a", 44100, 16, 2, 2.0});
    const auto second = testutil::generatedFile({"airplay-fade-b", 44100, 16, 2, 2.0});
    Settings settings;
    settings.crossfade.enabled = true;
    settings.crossfade.seconds = 1.0;
    auto server = serverFor(settings);
    server->submit(queue({first, second}));
    EXPECT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return now.queue.entries.size() == 2 && now.queueEntryId == now.queue.entries.back().id;
    })) << server->snapshot().playback.errorDetail;
    EXPECT_EQ(rtsp_.count("FLUSH"), 0);
    EXPECT_EQ(rtsp_.count("SETUP"), 1);
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, ADspChangeKeepsTheStreamGoing) {
    const auto file = testutil::generatedFile({"airplay-dsp", 44100, 16, 2, 4.0});
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() > 20; })) << server->snapshot().playback.errorDetail;
    auto changed = nativeSettings(server->snapshot().settings);
    changed.crossfeed.enabled = !changed.crossfeed.enabled;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(changed);
    server->submit(std::move(apply));
    server->waitIdle();
    const size_t sent = rtsp_.packetCount();
    EXPECT_TRUE(waitFor([&] { return rtsp_.packetCount() > sent + 20; }));
    EXPECT_EQ(rtsp_.count("FLUSH"), 0);
    EXPECT_FALSE(server->snapshot().playback.bitPerfect());
    server->shutdown();
}

TEST_F(AirPlayStreamingTest, OtherFormatsGoThroughTheConversionRule) {
    const auto file = testutil::generatedFile({"airplay-convert", 48000, 24, 2, 0.5});
    auto server = serverFor();
    server->submit(queue({file}));
    EXPECT_TRUE(waitFor([&] { return rtsp_.packetCount() > 10; })) << server->snapshot().playback.errorDetail;
    EXPECT_FALSE(server->snapshot().playback.bitPerfect());
    EXPECT_EQ(server->snapshot().playback.activeBackend, Backend::AirPlay);
    server->shutdown();
}

TEST(AirPlaySettingsTest, LevelsSurviveInTheSettingsFile) {
    Settings s;
    s.airplayLevels["800A805D016B"] = {12, true};
    const Settings read = fromJson(toJson(s));
    ASSERT_TRUE(read.airplayLevels.contains("800A805D016B"));
    EXPECT_EQ(read.airplayLevels.at("800A805D016B"), (AirPlayLevel{12, true}));
    EXPECT_TRUE(fromJson(toJson(Settings{})).airplayLevels.empty());
}

namespace {

// Receivers grouped in one stream, as the Server asks for them.
class AirPlayGroupOutputTest : public ::testing::Test {
protected:
    static AirPlayMember member(const FakeRtsp& rtsp, const std::string& id, AirPlayChannels channels = AirPlayChannels::Stereo,
                                std::optional<double> db = {}) {
        return {id, airplay::Receiver{id, id, "127.0.0.1", rtsp.port(), "105.1", airplay::Support::Supported, {}}, channels, db};
    }

    void want(std::vector<AirPlayMember> members) {
        std::scoped_lock lock(mutex_);
        members_ = std::move(members);
    }

    std::vector<std::pair<std::string, audio::ErrorCode>> left() {
        std::scoped_lock lock(mutex_);
        return left_;
    }

    std::unique_ptr<audio::IOutput> output() {
        AirPlayOutputOptions options;
        options.details = sampleDetails;
        options.members = [this] {
            std::scoped_lock lock(mutex_);
            return members_;
        };
        options.memberLeft = [this](const std::string& id, const audio::Error& e) {
            std::scoped_lock lock(mutex_);
            left_.emplace_back(id, e.code());
        };
        options.requestTimeoutMs = 500;
        options.heartbeat = 50ms;
        options.closeSessionAfter = 300ms;
        auto created = createAirPlayGroupOutput(options);
        created->setErrorHandler([this](const audio::Error& e) { error_ = int(e.code()) + 1; });
        return created;
    }

    static bool sameStream(const FakeRtsp& one, const FakeRtsp& other) {
        const auto a = one.packets();
        const auto b = other.packets();
        size_t matched = 0;
        for (const auto& packet : b) {
            const auto same = std::ranges::find(a, packet.sequence, &testutil::FakeRtp::sequence);
            if (same == a.end()) continue;
            if (same->rtpTime != packet.rtpTime || same->alac != packet.alac) return false;
            ++matched;
        }
        return matched >= 10;
    }

    static bool told(const FakeRtsp& rtsp, const std::string& text) {
        return waitFor([&] {
            return std::ranges::any_of(rtsp.requestsOf("SET_PARAMETER"),
                                       [&](const std::string& r) { return r.find(text) != std::string::npos; });
        });
    }

    FakeRtsp a_, b_;
    std::mutex mutex_;
    std::vector<AirPlayMember> members_;
    std::vector<std::pair<std::string, audio::ErrorCode>> left_;
    std::atomic<int> error_ = 0;
};

bool oneChannel(const testutil::FakeRtp& p) {
    const std::string pcm = pcmOfAlac(p.alac);
    for (size_t at = 0; at + 4 <= pcm.size(); at += 4)
        if (pcm[at] != pcm[at + 2] || pcm[at + 1] != pcm[at + 3]) return false;
    return !pcm.empty();
}

} // namespace

TEST_F(AirPlayGroupOutputTest, AReceiverAddedWhilePlayingJoinsTheSameStream) {
    want({member(a_, "A")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return a_.packetCount() >= 40; }));
    EXPECT_EQ(b_.count("RECORD"), 0);
    want({member(a_, "A"), member(b_, "B")});
    ASSERT_TRUE(waitFor([&] { return b_.packetCount() >= 40; }));
    EXPECT_EQ(a_.count("SETUP"), 1);
    EXPECT_EQ(a_.count("FLUSH"), 0);
    const auto joined = b_.packets();
    EXPECT_TRUE(joined.front().marker);
    // Packets sent while it was joining come after the sequence its RECORD named; it asks for those again.
    const std::string record = b_.requestsOf("RECORD").front();
    const size_t seq = record.find("RTP-Info: seq=");
    ASSERT_NE(seq, std::string::npos);
    const auto named = uint16_t(std::stoul(record.substr(seq + 14)));
    EXPECT_LT(uint16_t(joined.front().sequence - named), 50);
    EXPECT_TRUE(sameStream(a_, b_));
    ASSERT_FALSE(b_.controlPackets().empty());
    EXPECT_EQ(uint8_t(b_.controlPackets().front()[0]), 0x90);
    EXPECT_EQ(error_, 0);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, EveryReceiverIsSentTheSameClock) {
    want({member(a_, "A"), member(b_, "B")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return a_.controlPackets().size() >= 3 && b_.controlPackets().size() >= 3; }));
    out->close();
    const auto a = a_.controlPackets();
    const auto b = b_.controlPackets();
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_EQ(a[i].size(), 20U);
        EXPECT_EQ(a[i].substr(4), b[i].substr(4)) << i; // the same frame at the same NTP time
    }
    EXPECT_TRUE(sameStream(a_, b_));
}

TEST_F(AirPlayGroupOutputTest, AReceiverTakenOutIsLetGoAndTheOthersPlayOn) {
    want({member(a_, "A"), member(b_, "B")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return b_.packetCount() >= 20; }));
    want({member(a_, "A")});
    ASSERT_TRUE(waitFor([&] { return b_.count("TEARDOWN") == 1; }));
    const size_t sent = a_.packetCount();
    EXPECT_TRUE(waitFor([&] { return a_.packetCount() > sent + 20; }));
    EXPECT_EQ(a_.count("TEARDOWN"), 0);
    EXPECT_EQ(error_, 0);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, LeftAndRightEachFillBothChannels) {
    want({member(a_, "A", AirPlayChannels::Left), member(b_, "B", AirPlayChannels::Right)});
    CountingSource source(kSecond * 4);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return a_.packetCount() >= 20 && b_.packetCount() >= 20; }));
    out->close();
    const auto left = a_.packets(), right = b_.packets();
    for (size_t i = 0; i < 20; ++i) {
        const std::string l = pcmOfAlac(left[i].alac), r = pcmOfAlac(right[i].alac);
        ASSERT_EQ(l.size(), 1408U);
        ASSERT_EQ(r.size(), 1408U);
        for (size_t frame = 0; frame < 352; ++frame) {
            const uint64_t at = i * 1408 + frame * 4;
            for (size_t b = 0; b < 2; ++b) {
                ASSERT_EQ(uint8_t(l[frame * 4 + b]), CountingSource::byteAt(at + b)) << i << " " << frame;
                ASSERT_EQ(uint8_t(l[frame * 4 + 2 + b]), CountingSource::byteAt(at + b)) << i << " " << frame;
                ASSERT_EQ(uint8_t(r[frame * 4 + b]), CountingSource::byteAt(at + 2 + b)) << i << " " << frame;
                ASSERT_EQ(uint8_t(r[frame * 4 + 2 + b]), CountingSource::byteAt(at + 2 + b)) << i << " " << frame;
            }
        }
    }
}

TEST_F(AirPlayGroupOutputTest, AChannelChangeWhilePlayingReachesThatReceiverAlone) {
    want({member(a_, "A"), member(b_, "B")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return b_.packetCount() >= 10; }));
    want({member(a_, "A"), member(b_, "B", AirPlayChannels::Left)});
    ASSERT_TRUE(waitFor([&] { return oneChannel(b_.packets().back()); }));
    EXPECT_FALSE(oneChannel(a_.packets().back()));
    EXPECT_EQ(b_.count("FLUSH"), 0);
    EXPECT_EQ(b_.count("SETUP"), 1);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, APairPlaysBothChannelsWhateverItIsSet) {
    FakeRtsp second;
    AirPlayMember pair = member(a_, "PAIR", AirPlayChannels::Left);
    pair.receiver->others = {{"127.0.0.1", second.port()}};
    want({pair});
    CountingSource source(kSecond * 4);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return a_.packetCount() >= 10 && second.packetCount() >= 10; }));
    out->close();
    const std::string pcm = pcmOfAlac(a_.packets().front().alac);
    ASSERT_EQ(pcm.size(), 1408U);
    for (size_t b = 0; b < pcm.size(); ++b) ASSERT_EQ(uint8_t(pcm[b]), CountingSource::byteAt(b)) << b;
}

TEST_F(AirPlayGroupOutputTest, EachReceiverIsToldItsOwnLevel) {
    want({member(a_, "A", AirPlayChannels::Stereo, -10.0), member(b_, "B", AirPlayChannels::Stereo, -20.1)});
    CountingSource source(kSecond * 4);
    auto out = output();
    out->open(kCd, &source);
    EXPECT_TRUE(told(a_, "volume: -10.000000"));
    EXPECT_TRUE(told(b_, "volume: -20.100000"));
    for (const auto& request : a_.requestsOf("SET_PARAMETER")) EXPECT_EQ(request.find("-20.1"), std::string::npos);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, AReceiverTakenByAnotherSourceLeavesAndTheOthersPlayOn) {
    want({member(a_, "A"), member(b_, "B")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return b_.packetCount() >= 10; }));
    b_.takeOver();
    ASSERT_TRUE(waitFor([&] { return !left().empty(); }));
    EXPECT_EQ(left().front(), std::make_pair(std::string("B"), audio::ErrorCode::DeviceInUse));
    const size_t sent = a_.packetCount();
    EXPECT_TRUE(waitFor([&] { return a_.packetCount() > sent + 20; }));
    EXPECT_EQ(error_, 0);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, AReceiverThatCannotJoinIsToldAsLeaving) {
    b_.answer("SETUP", 453);
    want({member(a_, "A")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return a_.packetCount() >= 10; }));
    want({member(a_, "A"), member(b_, "B")});
    ASSERT_TRUE(waitFor([&] { return !left().empty(); }));
    EXPECT_EQ(left().front(), std::make_pair(std::string("B"), audio::ErrorCode::DeviceInUse));
    EXPECT_EQ(error_, 0);
    out->close();
}

TEST_F(AirPlayGroupOutputTest, OnlyTheLastReceiverLostEndsTheStream) {
    want({member(a_, "A"), member(b_, "B")});
    CountingSource source(kSecond * 10);
    auto out = output();
    out->open(kCd, &source);
    out->start();
    ASSERT_TRUE(waitFor([&] { return b_.packetCount() >= 10; }));
    b_.takeOver();
    ASSERT_TRUE(waitFor([&] { return !left().empty(); }));
    EXPECT_EQ(error_, 0);
    want({member(a_, "A")});
    a_.takeOver();
    EXPECT_TRUE(waitFor([&] { return error_ == int(audio::ErrorCode::DeviceInUse) + 1; }));
    out->close();
}

namespace {

// Two receivers found on the network, grouped through the Server.
class AirPlayGroupStreamingTest : public AirPlayStreamingTest {
protected:
    std::unique_ptr<LocalServer> groupServer(Settings settings = {}) {
        FakeReceiverRecord record;
        record.deviceId = "FAE2708FA125";
        record.name = "Bedroom (2)";
        record.host = "bedroom2";
        record.port = second_.port();
        mdns_.add(record);
        auto server = serverFor(std::move(settings));
        EXPECT_TRUE(waitFor([&] {
            Command again;
            again.kind = CommandKind::RefreshDevices;
            server->submit(std::move(again));
            server->waitIdle();
            return server->snapshot().devices.airplay.size() == 2;
        }));
        return server;
    }

    static void change(LocalServer& server, const std::function<void(Settings&, const DeviceCatalog&)>& edit) {
        auto s = nativeSettings(server.snapshot().settings);
        edit(s, server.snapshot().devices);
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(s);
        server.submit(std::move(apply));
        server.waitIdle();
    }

    static Command volume(uint32_t level, std::string receiver = {}) {
        Command c;
        c.kind = receiver.empty() ? CommandKind::SetPlayerVolume : CommandKind::SetReceiverVolume;
        c.playerVolume = level;
        c.receiver = std::move(receiver);
        return c;
    }

    static bool told(const FakeRtsp& rtsp, const std::string& text) {
        return waitFor([&] {
            return std::ranges::any_of(rtsp.requestsOf("SET_PARAMETER"),
                                       [&](const std::string& r) { return r.find(text) != std::string::npos; });
        });
    }

    FakeRtsp second_;
};

} // namespace

TEST_F(AirPlayGroupStreamingTest, BothReceiversPlayTheGroupAndItIsRemembered) {
    const auto file = testutil::generatedFile({"airplay-group", 44100, 16, 2, 4.0});
    auto server = groupServer();
    change(*server, [](Settings& s, const DeviceCatalog& d) { setInAirPlayGroup(s, "FAE2708FA125", true, d); });
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return rtsp_.packetCount() > 40 && second_.packetCount() > 40; }))
        << server->snapshot().playback.errorDetail;
    const auto playback = server->snapshot().playback;
    ASSERT_EQ(playback.receivers.size(), 2U);
    EXPECT_EQ(playback.receivers[0].id, "800A805D016B");
    EXPECT_EQ(playback.receivers[1].name, "Bedroom (2)");
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.outputMode.find("DMP-A6 + Bedroom (2)") != std::string::npos; }))
        << server->snapshot().playback.outputMode;
    EXPECT_EQ(nativeSettings(server->snapshot().settings).airplayGroup,
              (std::vector<std::string>{"800A805D016B", "FAE2708FA125"}));
    server->shutdown();
}

TEST_F(AirPlayGroupStreamingTest, TheGroupSliderMovesEveryReceiverInProportion) {
    const auto file = testutil::generatedFile({"airplay-group-volume", 44100, 16, 2, 4.0});
    auto server = groupServer();
    change(*server, [](Settings& s, const DeviceCatalog& d) { setInAirPlayGroup(s, "FAE2708FA125", true, d); });
    server->submit(volume(40, "800A805D016B"));
    server->submit(volume(20, "FAE2708FA125"));
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.playerVolume == 40U; }));
    server->submit(volume(80));
    ASSERT_TRUE(waitFor([&] {
        const auto r = server->snapshot().playback.receivers;
        return r.size() == 2 && r[0].level == 80 && r[1].level == 40;
    }));
    EXPECT_EQ(server->snapshot().playback.playerVolume, 80U);
    server->submit(queue({file}));
    EXPECT_TRUE(told(rtsp_, std::format("volume: {:.6f}", airplay::volumeDb(80, false))));
    EXPECT_TRUE(told(second_, std::format("volume: {:.6f}", airplay::volumeDb(40, false))));
    server->shutdown();
}

TEST_F(AirPlayGroupStreamingTest, ADragInManyStepsKeepsTheProportions) {
    auto server = groupServer();
    change(*server, [](Settings& s, const DeviceCatalog& d) { setInAirPlayGroup(s, "FAE2708FA125", true, d); });
    server->submit(volume(10, "800A805D016B"));
    server->submit(volume(5, "FAE2708FA125"));
    for (uint32_t level = 11; level <= 20; ++level) server->submit(volume(level));
    ASSERT_TRUE(waitFor([&] {
        const auto r = server->snapshot().playback.receivers;
        return r.size() == 2 && r[0].level == 20;
    }));
    EXPECT_EQ(server->snapshot().playback.receivers[1].level, 10U);
    server->shutdown();
}

TEST_F(AirPlayGroupStreamingTest, AReceiverTakenLeavesTheGroupSayingWhyAndTheOtherPlaysOn) {
    const auto file = testutil::generatedFile({"airplay-group-leave", 44100, 16, 2, 6.0});
    auto server = groupServer();
    change(*server, [](Settings& s, const DeviceCatalog& d) { setInAirPlayGroup(s, "FAE2708FA125", true, d); });
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return second_.packetCount() > 20; })) << server->snapshot().playback.errorDetail;
    second_.takeOver();
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.receiverLeft.has_value(); }));
    const auto left = *server->snapshot().playback.receiverLeft;
    EXPECT_EQ(left.name, "Bedroom (2)");
    EXPECT_EQ(left.code, audio::ErrorCode::DeviceInUse);
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.receivers.size() == 1; }));
    EXPECT_EQ(nativeSettings(server->snapshot().settings).airplayGroup, (std::vector<std::string>{"800A805D016B"}));
    const size_t sent = rtsp_.packetCount();
    EXPECT_TRUE(waitFor([&] { return rtsp_.packetCount() > sent + 20; }));
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Playing);
    server->shutdown();
}

TEST_F(AirPlayGroupStreamingTest, ChoosingAReceiverOfTheLastGroupPlaysToThemAll) {
    const auto file = testutil::generatedFile({"airplay-group-restore", 44100, 16, 2, 4.0});
    Settings settings;
    settings.airplayGroup = {"FAE2708FA125", "800A805D016B"};
    auto server = groupServer(settings);
    EXPECT_EQ(nativeSettings(server->snapshot().settings).airplayGroup.size(), 2U);
    server->submit(queue({file}));
    EXPECT_TRUE(waitFor([&] { return rtsp_.packetCount() > 20 && second_.packetCount() > 20; }))
        << server->snapshot().playback.errorDetail;
    server->shutdown();
}
