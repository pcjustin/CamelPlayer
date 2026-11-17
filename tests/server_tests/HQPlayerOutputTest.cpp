// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "FakeHQPlayer.h"
#include "Server/Network/HQPlayerOutput.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;
using audio::DeviceFormat;
using audio::SampleType;

namespace {

constexpr int kNoError = -1;

template <class Condition> bool eventually(Condition condition, std::chrono::milliseconds limit = 10000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

// Frames counting up, so any lost, repeated or reordered byte shows; a second track after a boundary.
class CountingSource final : public audio::IRenderSource {
public:
    CountingSource(uint32_t bytesPerFrame, uint64_t first, uint64_t second = 0)
        : bytesPerFrame_(bytesPerFrame), first_(first), second_(second) {}

    size_t render(uint8_t* destination, size_t frames) noexcept override {
        boundary_ = kNoBoundary;
        const uint64_t total = first_ + second_;
        const uint64_t at = at_.load();
        const size_t n = size_t(std::min<uint64_t>(frames, total - at));
        for (size_t i = 0; i < n * bytesPerFrame_; ++i) destination[i] = uint8_t(((at * bytesPerFrame_) + i) * 7 >> 3);
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
    std::string expected(uint64_t from, uint64_t frames) const {
        std::string bytes(size_t(frames * bytesPerFrame_), '\0');
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(uint8_t(((from * bytesPerFrame_) + i) * 7 >> 3));
        return bytes;
    }

private:
    uint32_t bytesPerFrame_;
    uint64_t first_, second_;
    std::atomic<uint64_t> at_ = 0;
    size_t boundary_ = kNoBoundary;
};

TrackDetails details(const std::wstring& file) {
    return {"Title & <more>", "Artist", "Album", file == L"first.flac" ? "jpeg bytes" : ""};
}

HQPlayerOutputOptions quick() {
    HQPlayerOutputOptions options;
    options.client.connectTimeout = 1000ms;
    options.client.firstReply = 3000ms;
    options.client.reply = 2000ms;
    options.ownChange = 500ms;
    options.playAgainAfter = 1000ms;
    return options;
}

class OutputRun {
public:
    OutputRun(testutil::FakeHQPlayer& fake, CountingSource& source, const DeviceFormat& format,
              HQPlayerOutputOptions options = quick())
        : output_(createHQPlayerOutput(fake.address(), details, [this](const PlayerEvent& e) { record(e); }, options)) {
        output_->setErrorHandler([this](const audio::Error& e) {
            std::scoped_lock lock(mutex_);
            error_ = int(e.code());
            detail_ = e.what();
        });
        output_->open(format, &source);
    }
    ~OutputRun() { output_->close(); }
    OutputRun(const OutputRun&) = delete;
    OutputRun& operator=(const OutputRun&) = delete;

    audio::IOutput* operator->() { return output_.get(); }
    int error() {
        std::scoped_lock lock(mutex_);
        return error_;
    }
    std::string detail() {
        std::scoped_lock lock(mutex_);
        return detail_;
    }

private:
    void record(const PlayerEvent&) {}
    std::unique_ptr<audio::IOutput> output_;
    std::mutex mutex_;
    int error_ = kNoError;
    std::string detail_;
};

uint32_t le32(const std::string& bytes, size_t at) {
    return *reinterpret_cast<const uint32_t*>(bytes.data() + at);
}

size_t count(const std::vector<std::string>& all, const std::string& one) {
    return size_t(std::ranges::count(all, one));
}

const DeviceFormat k16{44100, 2, SampleType::Int16};
const DeviceFormat k24{96000, 2, SampleType::Int24};

} // namespace

TEST(HQPlayerOutputTest, TakesWavAtAnyRateAndDsfAndSaysItIsHQPlayer) {
    testutil::FakeHQPlayer fake;
    auto output = createHQPlayerOutput(fake.address(), details, {}, quick());
    EXPECT_TRUE(output->supports(k16));
    EXPECT_TRUE(output->supports(DeviceFormat{192000, 2, SampleType::Int24}));
    EXPECT_TRUE(output->supports(DeviceFormat{2822400 / 8, 2, SampleType::Dsd8}));
    EXPECT_FALSE(output->supports(DeviceFormat{44100, 6, SampleType::Int16}));
    EXPECT_TRUE(output->gapless());
    EXPECT_EQ(output->status().mode, "HQPlayer · " + fake.address());
}

TEST(HQPlayerOutputTest, TheSamplesHQPlayerFetchesAreTheSourcesBitForBit) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k24.bytesPerFrame(), 40000);
    OutputRun output(fake, source, k24);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1; }));
    const std::string body = fake.received().begin()->second;
    ASSERT_EQ(body.size(), 44 + 40000U * 6);
    EXPECT_EQ(body.substr(0, 4), "RIFF");
    EXPECT_EQ(le32(body, 24), 96000U);
    EXPECT_TRUE(body.compare(44, std::string::npos, source.expected(0, 40000)) == 0);
    EXPECT_EQ(output.error(), kNoError);
}

TEST(HQPlayerOutputTest, RawDsdGoesAsDsf) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    const DeviceFormat dsd{352800, 2, SampleType::Dsd8};
    CountingSource source(2, 5000);
    OutputRun output(fake, source, dsd);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1; }));
    const std::string body = fake.received().begin()->second;
    ASSERT_EQ(body.size(), 92 + 2 * 4096U * 2);
    EXPECT_EQ(body.substr(0, 4), "DSD ");
    EXPECT_EQ(le32(body, 56), 352800U * 8);
}

// A new playlist starts with Stop and PlaylistClear, since a clear while paused keeps the paused track; Play goes once
// the track is added.
TEST(HQPlayerOutputTest, ATrackIsAddedToAClearedPlaylistWithItsTitleArtistAlbumAndCover) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 20000);
    OutputRun output(fake, source, k16);
    const size_t connected = fake.commands().size();
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1; }));
    auto commands = fake.commands();
    commands.erase(commands.begin(), commands.begin() + std::ptrdiff_t(connected));
    std::erase(commands, "Status");
    ASSERT_GE(commands.size(), 4U);
    EXPECT_EQ(std::vector<std::string>(commands.begin(), commands.begin() + 4),
              (std::vector<std::string>{"Stop", "PlaylistClear", "PlaylistAdd", "Play"}));
    const auto raw = fake.raw();
    const auto add = std::ranges::find_if(raw, [](const std::string& r) { return r.find("<PlaylistAdd") != std::string::npos; });
    ASSERT_NE(add, raw.end());
    EXPECT_NE(add->find(R"(song="Title &amp; &lt;more&gt;")"), std::string::npos) << *add;
    EXPECT_NE(add->find(R"(artist="Artist")"), std::string::npos);
    EXPECT_NE(add->find(R"(album="Album")"), std::string::npos);
    EXPECT_NE(add->find(R"(cover="http://127.0.0.1:)"), std::string::npos);
    EXPECT_NE(add->find(R"(clear="0")"), std::string::npos);
}

// HQPlayer's HEAD, its probe broken off and its second opening of the same URL are all served.
TEST(HQPlayerOutputTest, TheProbeAndTheSecondFetchOfAUrlAreServed) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 3);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1; }));
    const std::string body = fake.received().begin()->second;
    ASSERT_EQ(body.size(), 44 + 44100U * 3 * 4);
    EXPECT_TRUE(body.compare(44, std::string::npos, source.expected(0, 44100ULL * 3)) == 0);
    EXPECT_EQ(output.error(), kNoError);
}

TEST(HQPlayerOutputTest, TheNextTrackJoinsThePlaylistAndPlaysWithoutAGap) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 30000, 20000);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 2; }));
    const auto added = fake.added();
    ASSERT_EQ(added.size(), 2U);
    const auto received = fake.received();
    EXPECT_TRUE(received.at(added[0]).compare(44, std::string::npos, source.expected(0, 30000)) == 0);
    EXPECT_TRUE(received.at(added[1]).compare(44, std::string::npos, source.expected(30000, 20000)) == 0);
    EXPECT_EQ(count(fake.commands(), "PlaylistClear"), 2U); // the connection's and the first track's, none between
    EXPECT_EQ(output.error(), kNoError);
}

TEST(HQPlayerOutputTest, APauseFromHQPlayersOwnWindowIsAnotherSource) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().state == 2 && fake.state().position > 0.5; }));
    fake.change([](auto& s) { s.state = 1; });
    ASSERT_TRUE(eventually([&] { return output.error() == int(audio::ErrorCode::DeviceInUse); }));
    EXPECT_EQ(output.detail(), "HQPlayer is in use by another source");
}

TEST(HQPlayerOutputTest, OurOwnPauseAndPlayAreNotAnotherSource) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    output->pause();
    std::this_thread::sleep_for(800ms);
    output->start();
    std::this_thread::sleep_for(800ms);
    EXPECT_EQ(fake.state().state, 2);
    EXPECT_EQ(output.error(), kNoError);
}

TEST(HQPlayerOutputTest, AnOutputThatCannotOpenIsReportedAfterPlayIsSentOnceMore) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.outputFails = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return output.error() != kNoError; }, 15000ms));
    EXPECT_EQ(output.error(), int(audio::ErrorCode::DriverError));
    EXPECT_NE(output.detail().find("HQPlayer could not open its output"), std::string::npos);
    EXPECT_EQ(count(fake.commands(), "Play"), 2U);
}

// Stopped some seconds after Play without its position ever moving is still an output that did not open.
TEST(HQPlayerOutputTest, AnOutputThatStopsSecondsAfterPlayWithoutPlayingDidNotOpen) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.outputFails = true;
    fake.outputFailsAfter = 3500ms;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return output.error() != kNoError; }, 15000ms));
    EXPECT_EQ(output.error(), int(audio::ErrorCode::DriverError));
    EXPECT_NE(output.detail().find("HQPlayer could not open its output"), std::string::npos);
}

TEST(HQPlayerOutputTest, AnHQPlayerThatQuitsIsNotConnected) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    fake.dropConnections();
    ASSERT_TRUE(eventually([&] { return output.error() == int(audio::ErrorCode::DeviceLost); }));
    EXPECT_NE(output.detail().find("30-minute trial"), std::string::npos);
}

TEST(HQPlayerOutputTest, ClosingStopsHQPlayer) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    {
        OutputRun output(fake, source, k16);
        output->start();
        ASSERT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    }
    EXPECT_EQ(fake.state().state, 0);
    EXPECT_EQ(fake.commands().back(), "Stop");
}

TEST(HQPlayerOutputTest, WhatHQPlayerMakesOfTheStreamIsTheOutputsDetail) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.change([](auto& s) {
        s.activeRate = 384000;
        s.activeBits = 32;
    });
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return !output->status().detail.empty(); }));
    EXPECT_EQ(output->status().detail, "384 kHz/32-bit · poly-sinc-gauss-long · TPDF");
    EXPECT_EQ(describeProcessing({}), "");
    EXPECT_EQ(describeProcessing({.rate = 352800, .bits = 32}), "352.8 kHz/32-bit");
}

// A seek or a track chosen is a new stream; HQPlayer keeps the one connection, whose first reply takes seconds.
TEST(HQPlayerOutputTest, EveryStreamUsesTheOneConnection) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource first(k16.bytesPerFrame(), 20000), second(k16.bytesPerFrame(), 20000);
    auto output = createHQPlayerOutput(fake.address(), details, {}, quick());
    output->open(k16, &first);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1; }));
    output->close();
    output->open(k16, &second);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 2; }));
    output->close();
    EXPECT_EQ(fake.accepted(), 1U);
}

// HQPlayer says nothing for seconds while it starts DSD; its next Status shows CamelPlayer's own playlist edits.
TEST(HQPlayerOutputTest, AStartSilentForSecondsIsNotAnotherSource) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.quietAfterPlay = 2500ms;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    std::this_thread::sleep_for(4000ms);
    EXPECT_EQ(output.error(), kNoError);
    EXPECT_EQ(fake.state().state, 2);
}

// At the end of its playlist HQPlayer moves its track serial and stops: the end, not another source.
TEST(HQPlayerOutputTest, TheEndOfThePlaylistIsNotAnotherSource) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.received().size() == 1 && fake.state().state == 0; }));
    std::this_thread::sleep_for(1000ms);
    EXPECT_EQ(output.error(), kNoError);
}

// Next pressed in HQPlayer's window, mid-track, is another source.
TEST(HQPlayerOutputTest, ATrackChangedMidTrackFromHQPlayersWindowIsAnotherSource) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 1.0; }));
    fake.change([](auto& s) {
        ++s.trackSerial;
        s.position = 0;
    });
    ASSERT_TRUE(eventually([&] { return output.error() == int(audio::ErrorCode::DeviceInUse); }));
}

TEST(HQPlayerOutputTest, DsdIsDescribedByItsRate) {
    EXPECT_EQ(describeProcessing({.rate = 11289600, .bits = 1, .filter = "poly-sinc-gauss-hires-lp", .shaper = "ASDM7EC-fast"}),
              "DSD256 · poly-sinc-gauss-hires-lp · ASDM7EC-fast");
}

// A new stream starts at 0 though the last Status still shows where the stream before it was: its Play is not sent again.
TEST(HQPlayerOutputTest, ANewStreamIsNotTakenForOneStandingStill) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource first(k16.bytesPerFrame(), 44100ULL * 60), second(k16.bytesPerFrame(), 44100ULL * 60);
    auto output = createHQPlayerOutput(fake.address(), details, {}, quick());
    output->open(k16, &first);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 2.0; }));
    output->close();
    output->open(k16, &second);
    output->start();
    std::this_thread::sleep_for(2500ms);
    EXPECT_EQ(count(fake.commands(), "Play"), 2U);
    output->close();
}

// A reply missing while a new track is loaded connects again, which clears the playlist as the load does: it plays.
TEST(HQPlayerOutputTest, AReplyMissingWhileATrackLoadsIsNoLoss) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    fake.dropNext("PlaylistClear");
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    EXPECT_EQ(output.error(), kNoError);
    EXPECT_EQ(fake.accepted(), 2U);
}

// HQPlayer on macOS answers the add of a DSF only once it has read some of its samples, before Play is sent.
TEST(HQPlayerOutputTest, ATrackIsAddedWhenHQPlayerReadsSamplesBeforeAnswering) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.readBeforeAddReply = size_t{4} * 4096;
    const DeviceFormat dsd{352800, 2, SampleType::Dsd8};
    CountingSource source(2, 352800ULL * 10);
    OutputRun output(fake, source, dsd);
    EXPECT_NO_THROW(output->start());
    EXPECT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    EXPECT_EQ(output.error(), kNoError);
    EXPECT_EQ(fake.accepted(), 1U);
}

// HQPlayer on macOS took over 15 s to answer Stop while it stopped DSD, and moved its track meanwhile (2026-11-17).
// HQPlayer reports its position from the start: while it stays at 0 starting DSD, nothing counts as played, rather than
// the time since Play as for a player that never reports one.
TEST(HQPlayerOutputTest, NothingCountsAsPlayedWhileHQPlayerStaysAtItsStart) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    fake.positionHeldFor = 7000ms;
    const uint64_t frames = 44100ULL * 10;
    CountingSource source(k16.bytesPerFrame(), frames);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return output->status().heldFrames == frames; }));
    std::this_thread::sleep_for(5000ms);
    EXPECT_EQ(fake.state().position, 0.0);
    EXPECT_EQ(output->status().heldFrames, frames);
}

TEST(HQPlayerOutputTest, ClosingWhileHQPlayerIsSlowToStopEndsWithoutAnError) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(fake, source, k16);
    output->start();
    ASSERT_TRUE(eventually([&] { return fake.state().position > 0.5; }));
    fake.dropNext("Stop");
    auto closing = std::async(std::launch::async, [&] { output->close(); });
    std::this_thread::sleep_for(1000ms);
    fake.change([](testutil::FakeHQPlayer::State& s) { ++s.trackSerial; });
    EXPECT_EQ(closing.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(output.error(), kNoError);
}
