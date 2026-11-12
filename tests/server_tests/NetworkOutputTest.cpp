// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "Common/util/Text.h"
#include "FakeRenderer.h"
#include "Server/Network/NetworkOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "Server/Support/Http.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;
using audio::DeviceFormat;
using audio::SampleType;
using testutil::FakeRenderer;
using testutil::RendererKind;

namespace {

bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return check();
}

upnp::PlayerDescription describe(const FakeRenderer& renderer) {
    auto description = upnp::parseDescription(renderer.descriptionXml(), renderer.location());
    if (!description) throw std::logic_error("the fake's description does not parse");
    return *description;
}

// Frames counting up, so any lost, repeated or reordered byte shows. A boundary splits them into two tracks.
class CountingSource final : public audio::IRenderSource {
public:
    // `told` is the first track's length as the Engine tells it, when that is not the length it renders.
    CountingSource(uint32_t bytesPerFrame, uint64_t first, uint64_t second = 0, uint64_t told = 0)
        : bytesPerFrame_(bytesPerFrame), first_(first), second_(second), told_(told ? told : first) {}

    size_t render(uint8_t* destination, size_t frames) noexcept override {
        boundary_ = kNoBoundary;
        const uint64_t second = second_.load();
        const uint64_t total = first_ + second;
        const uint64_t at = at_.load();
        const size_t n = size_t(std::min<uint64_t>(frames, total - at));
        for (size_t i = 0; i < n * bytesPerFrame_; ++i) destination[i] = uint8_t(((at * bytesPerFrame_) + i) * 7 >> 3);
        if (second && at < first_ && at + n >= first_) boundary_ = size_t(first_ - at);
        at_ += n;
        return n;
    }
    size_t boundaryInLastRender() const noexcept override { return boundary_; }
    bool renderEnded() const noexcept override { return at_.load() == first_ + second_.load(); }
    audio::StreamTrack streamTrack() const override { return {L"first.flac", told_, 0, 1}; }
    std::optional<audio::StreamTrack> nextStreamTrack() const override {
        const uint64_t second = second_.load();
        if (!second || !queued_) return std::nullopt;
        return audio::StreamTrack{L"second.flac", second, 0, 2};
    }
    void queueSecond() { queued_ = true; }
    // As the Engine taking back the queued track before any of it was rendered.
    void withdrawSecond() {
        queued_ = false;
        second_ = 0;
    }
    uint64_t rendered() const { return at_.load(); }

    std::string expected(uint64_t from, uint64_t frames) const {
        std::string bytes(size_t(frames * bytesPerFrame_), '\0');
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(uint8_t(((from * bytesPerFrame_) + i) * 7 >> 3));
        return bytes;
    }

private:
    uint32_t bytesPerFrame_;
    uint64_t first_;
    std::atomic<uint64_t> second_;
    uint64_t told_;
    std::atomic<uint64_t> at_ = 0;
    size_t boundary_ = kNoBoundary;
    std::atomic<bool> queued_ = false;
};

TrackDetails sampleDetails(const std::wstring& file) {
    return {"Title & <more>", "Artist", "Album", file == L"first.flac" ? "jpeg bytes" : ""};
}

class OutputRun {
public:
    OutputRun(FakeRenderer& renderer, CountingSource& source, const DeviceFormat& format, NetworkOutputOptions options = {})
        : output_(createNetworkOutput(
              describe(renderer), sampleDetails, [this](const PlayerEvent& event) { record(event); }, options)) {
        output_->open(format, &source);
    }
    ~OutputRun() { output_->close(); }
    OutputRun(const OutputRun&) = delete;
    OutputRun& operator=(const OutputRun&) = delete;

    audio::IOutput& operator*() { return *output_; }
    audio::IOutput* operator->() { return output_.get(); }
    std::vector<PlayerEvent> events() {
        std::scoped_lock lock(mutex_);
        return events_;
    }

private:
    void record(const PlayerEvent& event) {
        std::scoped_lock lock(mutex_);
        events_.push_back(event);
    }
    std::unique_ptr<audio::IOutput> output_;
    std::mutex mutex_;
    std::vector<PlayerEvent> events_;
};

const DeviceFormat k16{44100, 2, SampleType::Int16};
const DeviceFormat k24{96000, 2, SampleType::Int24};

uint32_t le32(const std::string& bytes, size_t at) {
    return *reinterpret_cast<const uint32_t*>(bytes.data() + at);
}

} // namespace

TEST(NetworkOutputTest, TellsWhatThePlayerCanTake) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    auto output = createNetworkOutput(describe(renderer), sampleDetails, {});
    EXPECT_TRUE(output->supports(k16));
    EXPECT_TRUE(output->supports(k24));
    EXPECT_FALSE(output->supports({44100, 2, SampleType::Int32}));
    EXPECT_FALSE(output->supports({44100, 2, SampleType::Float32}));
    EXPECT_FALSE(output->supports({352800, 2, SampleType::Dsd8}));
    EXPECT_TRUE(output->gapless());
}

// A player that could not tell its formats when the output was made, as one waking from standby, is asked again.
TEST(NetworkOutputTest, APlayerThatCouldNotTellItsFormatsIsAskedAgain) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.refuseSoap(true);
    auto output = createNetworkOutput(describe(renderer), sampleDetails, {});
    renderer.refuseSoap(false);
    EXPECT_TRUE(output->supports(k16));
    EXPECT_TRUE(output->supports(k24));
    EXPECT_TRUE(output->gapless());
}

TEST(NetworkOutputTest, DsdOnlyWhenThePlayerDeclaresIt) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.declare("http-get:*:audio/wav:*,http-get:*:audio/x-dsf:*");
    EXPECT_TRUE(createNetworkOutput(describe(renderer), sampleDetails, {})->supports({352800, 2, SampleType::Dsd8}));
}

TEST(NetworkOutputTest, APlayerWithoutTheNextActionIsNotGapless) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.withoutNextTrack();
    auto output = createNetworkOutput(describe(renderer), sampleDetails, {});
    EXPECT_FALSE(output->gapless());
    EXPECT_FALSE(output->status().adjustments.empty());
}

TEST(NetworkOutputTest, APlayerThatReportsPlayingAfterAStopIsNotFollowed) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.startSlowly(2000);
    CountingSource source(k16.bytesPerFrame(), 44100);
    OutputRun output(renderer, source, k16);
    output->start();
    output->close();
    renderer.emitLastChange(R"(<TransportState val="STOPPED"/>)");
    renderer.emitLastChange(R"(<TransportState val="PLAYING"/>)");
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(std::ranges::count(output.events(), PlayerEvent{PlayerEventKind::Playing}), 0);
}

TEST(NetworkOutputTest, GaplessTurnedOffGivesThePlayerOneTrackAtATime) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    NetworkOutputOptions options;
    options.gapless = false;
    CountingSource source(k16.bytesPerFrame(), 2000, 2000);
    OutputRun output(renderer, source, k16, options);
    EXPECT_FALSE(output->gapless());
    EXPECT_TRUE(output->status().adjustments.empty()); // the player can, so nothing is wrong
    source.queueSecond();
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    std::this_thread::sleep_for(100ms);
    const auto actions = renderer.actions();
    EXPECT_EQ(std::ranges::count(actions, "SetNextAVTransportURI"), 0);
}

TEST(NetworkOutputTest, ServesTheFramesBitForBitAsWav) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    CountingSource source(k24.bytesPerFrame(), 40000);
    OutputRun output(renderer, source, k24);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    const std::string body = renderer.received().begin()->second;
    ASSERT_EQ(body.size(), 44 + 40000U * 6);
    EXPECT_EQ(body.substr(0, 4), "RIFF");
    EXPECT_EQ(le32(body, 24), 96000U);
    EXPECT_EQ(le32(body, 40), 40000U * 6);
    EXPECT_TRUE(body.compare(44, std::string::npos, source.expected(0, 40000)) == 0);
    EXPECT_NE(renderer.fetchHeaders().find("Content-Type: audio/wav"), std::string::npos);
}

TEST(NetworkOutputTest, FallsBackToBigEndianL16WithoutWav) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.declare("http-get:*:audio/L16:*");
    CountingSource source(k16.bytesPerFrame(), 2000);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    std::string expected = source.expected(0, 2000);
    for (size_t i = 0; i + 1 < expected.size(); i += 2) std::swap(expected[i], expected[i + 1]);
    EXPECT_TRUE(renderer.received().begin()->second == expected);
    EXPECT_NE(renderer.fetchHeaders().find("audio/L16;rate=44100;channels=2"), std::string::npos);
}

TEST(NetworkOutputTest, SendsRawDsdAsDsfBlocks) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.declare("http-get:*:audio/x-dsf:*");
    const DeviceFormat dsd{352800, 2, SampleType::Dsd8};
    constexpr uint64_t frames = 5000;
    CountingSource source(2, frames);
    OutputRun output(renderer, source, dsd);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    const std::string body = renderer.received().begin()->second;
    ASSERT_EQ(body.size(), 92 + 2 * 4096U * 2);
    EXPECT_EQ(body.substr(0, 4), "DSD ");
    EXPECT_EQ(body.substr(28, 4), "fmt ");
    EXPECT_EQ(le32(body, 56), 352800U * 8);
    EXPECT_EQ(*reinterpret_cast<const uint64_t*>(body.data() + 64), frames * 8);
    EXPECT_EQ(le32(body, 72), 4096U);
    EXPECT_EQ(body.substr(80, 4), "data");
    const auto reversed = [](uint8_t value) {
        uint8_t out = 0;
        for (int bit = 0; bit < 8; ++bit)
            if (value & (1 << bit)) out |= uint8_t(0x80 >> bit);
        return out;
    };
    const std::string raw = source.expected(0, frames);
    EXPECT_EQ(uint8_t(body[92]), reversed(uint8_t(raw[0])));
    EXPECT_EQ(uint8_t(body[92 + 4095]), reversed(uint8_t(raw[size_t{4095} * 2])));
    EXPECT_EQ(uint8_t(body[92 + 4096]), reversed(uint8_t(raw[1])));
    EXPECT_EQ(uint8_t(body[92 + 8192]), reversed(uint8_t(raw[size_t{4096} * 2])));
    EXPECT_EQ(uint8_t(body[92 + 8192 + 903]), reversed(uint8_t(raw[size_t{4999} * 2])));
    EXPECT_EQ(body[92 + 8192 + 904], 0); // the last block's padding
}

TEST(NetworkOutputTest, DescribesTheTrackToThePlayer) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    CountingSource source(k16.bytesPerFrame(), 88200);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.currentUri().empty(); }));
    const auto didl = renderer.metadata();
    const auto parsed = upnp::parseXml(didl);
    ASSERT_TRUE(parsed) << didl;
    const auto* item = parsed->child("item");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->childText("title"), "Title & <more>");
    EXPECT_EQ(item->childText("artist"), "Artist");
    EXPECT_EQ(item->childText("album"), "Album");
    EXPECT_EQ(item->child("res")->attributes.at("duration"), "0:00:02.000");
    EXPECT_EQ(item->child("res")->text, renderer.currentUri());
    const auto cover = item->childText("albumArtURI");
    ASSERT_FALSE(cover.empty());
    const auto reply = http::send("GET", cover, "", "", 2000, 4096);
    EXPECT_EQ(reply.status, 200U);
    EXPECT_EQ(reply.body, "jpeg bytes");
}

TEST(NetworkOutputTest, OneUrlForEachTrackWithTheBoundaryExactlyWhereItBelongs) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    CountingSource source(k16.bytesPerFrame(), 30000, 20000);
    source.queueSecond();
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.played().size() == 2; }));
    const auto played = renderer.played();
    EXPECT_NE(played[0], played[1]);
    const auto received = renderer.received();
    EXPECT_TRUE(received.at(played[0]).compare(44, std::string::npos, source.expected(0, 30000)) == 0);
    EXPECT_TRUE(received.at(played[1]).compare(44, std::string::npos, source.expected(30000, 20000)) == 0);
    EXPECT_EQ(le32(received.at(played[1]), 40), 20000U * 4);
    const auto actions = renderer.actions();
    EXPECT_NE(std::ranges::find(actions, "SetNextAVTransportURI"), actions.end());
}

// A track told shorter than it renders: the player gets the length it was told, and the next track its own audio.
// A connection reset before it is served, as a port probe's, leaves the next track served.
TEST(NetworkOutputTest, AConnectionResetBeforeItIsServedLeavesTheStreamServed) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    CountingSource source(k16.bytesPerFrame(), 30000, 20000);
    source.queueSecond();
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.currentUri().empty(); }));
    for (int i = 0; i < 20; ++i) FakeRenderer::resetConnectionTo(renderer.currentUri());
    EXPECT_TRUE(waitFor([&] { return renderer.played().size() == 2; }));
}

TEST(NetworkOutputTest, ATrackToldTooShortLeavesTheNextTrackItsOwnBody) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    CountingSource source(k16.bytesPerFrame(), 30000, 20000, 25000);
    source.queueSecond();
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.played().size() == 2; }));
    const auto played = renderer.played();
    const auto received = renderer.received();
    EXPECT_TRUE(received.at(played[0]).compare(44, std::string::npos, source.expected(0, 25000)) == 0);
    EXPECT_EQ(le32(received.at(played[1]), 40), 20000U * 4);
    EXPECT_TRUE(received.at(played[1]).compare(44, std::string::npos, source.expected(30000, 20000)) == 0);
}

TEST(NetworkOutputTest, AnOpenHomePlayerGetsTheQueueAsItsPlaylist) {
    FakeRenderer renderer(RendererKind::OpenHome);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    CountingSource source(k16.bytesPerFrame(), 30000, 20000);
    source.queueSecond();
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.played().size() == 2; }));
    const auto actions = renderer.actions();
    EXPECT_EQ(std::ranges::count(actions, "Insert"), 2);
    EXPECT_NE(std::ranges::find(actions, "SeekId"), actions.end());
}

TEST(NetworkOutputTest, StaysAFewSecondsAheadOfThePlayer) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 600);
    NetworkOutputOptions options;
    options.ahead = 1000ms;
    OutputRun output(renderer, source, k16, options);
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    std::this_thread::sleep_for(1500ms);
    EXPECT_LT(source.rendered(), 44100ULL * 8); // far less than the 600 s it could have rendered
    EXPECT_GT(output->status().heldFrames, 0U);
}

TEST(NetworkOutputTest, ThePlayerHearsOfTheNextTrackOnlyOnceThatTrackHasData) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    constexpr uint64_t first = 44100ULL * 6;
    CountingSource source(k16.bytesPerFrame(), first, 44100ULL * 60);
    source.queueSecond();
    NetworkOutputOptions options;
    options.ahead = 1000ms;
    options.nextTrackLead = 3000ms;
    OutputRun output(renderer, source, k16, options);
    output->start();
    const auto told = [&] { return std::ranges::count(renderer.actions(), "SetNextAVTransportURI"); };
    std::this_thread::sleep_for(1000ms);
    EXPECT_EQ(told(), 0);
    EXPECT_TRUE(waitFor([&] { return told() == 1; }, 5s));
    EXPECT_GE(source.rendered(), first);
}

TEST(NetworkOutputTest, ANextTrackTakenBackIsNeitherFilledNorAnnounced) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL); // real time
    constexpr uint64_t first = 44100ULL * 4;
    CountingSource source(k16.bytesPerFrame(), first, 44100ULL * 2);
    source.queueSecond();
    NetworkOutputOptions options;
    options.ahead = 1000ms;
    options.aheadBeforePlay = 1000ms;
    options.nextTrackLead = 1000ms;
    OutputRun output(renderer, source, k16, options);
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    std::this_thread::sleep_for(300ms); // the queued track has its body by now
    ASSERT_LT(source.rendered(), first);
    source.withdrawSecond();
    ASSERT_TRUE(waitFor([&] { return source.rendered() == first; }, 10s));
    std::this_thread::sleep_for(6s); // the first plays out
    EXPECT_EQ(renderer.played().size(), 1U);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetNextAVTransportURI"), 0);
}

TEST(NetworkOutputTest, TheNextTrackIsFetchableAheadOnceTheFirstIsSent) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);         // real time
    renderer.fillNextTrack(k16.bytesPerFrame() * 44100ULL * 20); // more than the lead, which then sets the limit
    constexpr uint64_t first = 44100ULL * 1;
    CountingSource source(k16.bytesPerFrame(), first, 44100ULL * 600);
    source.queueSecond();
    NetworkOutputOptions options;
    options.ahead = 1000ms;
    options.aheadBeforePlay = 1000ms;
    options.nextTrackLead = 10000ms;
    OutputRun output(renderer, source, k16, options);
    output->start();
    // Well before the first has played, the stream has run on into the next, past what `ahead` allows alone.
    EXPECT_TRUE(waitFor([&] { return source.rendered() >= first + 44100ULL * 9; }, 3s)) << source.rendered();
    EXPECT_LT(source.rendered(), first + 44100ULL * 15);
}

TEST(NetworkOutputTest, APlayerThatNeverReportsAPositionIsTakenToPlayInRealTime) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.reportNoPosition();
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 44100ULL * 4; }));
    EXPECT_GT(output->status().heldFrames, 0U);
    EXPECT_TRUE(waitFor([&] { return source.rendered() > 44100ULL * 8; }, 7s)); // rendering goes on as time passes
}

// The clock that stands in for such a player's position stops while it is paused: after a pause of three seconds it
// does not count them as played.
TEST(NetworkOutputTest, TheStandInClockStopsWhileThePlayerIsPaused) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.reportNoPosition();
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    output->start();
    // Past the slow start the clock allows, once it counts a second played.
    ASSERT_TRUE(waitFor([&] { return output->status().heldFrames + 44100ULL < source.rendered(); }, 10s));
    output->pause();
    const auto heldWhenPaused = output->status().heldFrames;
    std::this_thread::sleep_for(3s);
    output->start();
    const auto heldWhenResumed = output->status().heldFrames;
    EXPECT_GE(double(heldWhenResumed), double(heldWhenPaused) - 44100.0 * 0.5)
        << "the pause was counted as played: " << heldWhenPaused << " held when paused, " << heldWhenResumed << " after";
}

TEST(NetworkOutputTest, APlayerThatReconnectsGetsTheStartAgain) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    CountingSource source(k16.bytesPerFrame(), 20000);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    const auto url = renderer.received().begin()->first;
    const auto again = http::send("GET", url, "", "", 2000, 1 << 20);
    EXPECT_EQ(again.status, 200U);
    EXPECT_EQ(again.body, renderer.received().begin()->second);
}

TEST(NetworkOutputTest, ARangeRequestGetsThatPartOfTheStream) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    CountingSource source(k16.bytesPerFrame(), 20000);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.received().empty(); }));
    const auto url = renderer.received().begin()->first;
    const auto part = http::send("GET", url, "Range: bytes=1044-2043", "", 2000, 1 << 20);
    EXPECT_EQ(part.status, 206U);
    EXPECT_EQ(part.body, renderer.received().begin()->second.substr(1044, 1000));
    EXPECT_NE(part.headers.find("Content-Range: bytes 1044-2043/80044"), std::string::npos);
    const auto outside = http::send("GET", url, "Range: bytes=900000-", "", 2000, 1 << 20);
    EXPECT_EQ(outside.status, 416U);
    const auto headOnly = http::send("HEAD", url, "", "", 2000, 1 << 20);
    EXPECT_EQ(headOnly.status, 200U);
    EXPECT_TRUE(headOnly.body.empty());
}

TEST(NetworkOutputTest, ARangePastTheStreamIsThePlayersOwnSeek) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(100000);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 3600);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return !renderer.currentUri().empty() && source.rendered() > 0; }));
    const uint64_t offset = 44 + uint64_t(k16.bytesPerFrame()) * 44100 * 1800;
    const auto seek = http::send("GET", renderer.currentUri(), "Range: bytes=" + std::to_string(offset) + "-", "", 2000, 1 << 20);
    EXPECT_EQ(seek.status, 503U);
    const auto events = output.events();
    const auto found = std::ranges::find_if(events, [](const PlayerEvent& e) { return e.kind == PlayerEventKind::Seeked; });
    ASSERT_NE(found, events.end());
    EXPECT_EQ(found->positionMs, 1800000U);
}

TEST(NetworkOutputTest, ARefusedStreamIsAFormatProblem) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.refuseStreams();
    CountingSource source(k16.bytesPerFrame(), 20000);
    OutputRun output(renderer, source, k16);
    try {
        output->start();
        FAIL() << "the player accepted it";
    } catch (const audio::Error& e) {
        EXPECT_EQ(e.code(), audio::ErrorCode::FormatNotSupported);
    }
}

TEST(NetworkOutputTest, APlayerThatIsGoneIsNotConnected) {
    auto description = [] {
        FakeRenderer renderer(RendererKind::UpnpAv);
        return describe(renderer);
    }();
    CountingSource source(4, 1000);
    auto output = createNetworkOutput(description, sampleDetails, {});
    try {
        output->open(k16, &source);
        FAIL() << "opened";
    } catch (const audio::Error& e) {
        EXPECT_EQ(e.code(), audio::ErrorCode::DeviceNotFound);
    }
}

namespace {

constexpr int kNoError = -1;

// The actions sent from `from` on that only ask the player how it is.
std::vector<std::string> controlsSince(const FakeRenderer& renderer, size_t from) {
    std::vector<std::string> controls;
    const auto actions = renderer.actions();
    for (size_t i = from; i < actions.size(); ++i)
        if (!actions[i].starts_with("Get") && actions[i] != "TransportState" && actions[i] != "Id" && actions[i] != "Time" &&
            actions[i] != "SourceIndex")
            controls.push_back(actions[i]);
    return controls;
}

} // namespace

TEST(NetworkOutputTest, ATakeoverSendsThePlayerNothingMore) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    const size_t before = renderer.actions().size();
    renderer.takeOver("http://other-app/song");
    ASSERT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DeviceInUse); }));
    output->pause();
    output->close();
    EXPECT_TRUE(controlsSince(renderer, before).empty()) << controlsSince(renderer, before).front();
    EXPECT_EQ(renderer.state(), "PLAYING");
}

TEST(NetworkOutputTest, APlayerBackAfterItWasLostIsFollowedAgain) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    renderer.refuseSoap(true);
    ASSERT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DeviceLost); }));
    renderer.refuseSoap(false);
    output->close();
    CountingSource again(k16.bytesPerFrame(), 44100ULL * 60);
    output->open(k16, &again);
    output->start();
    renderer.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(output.events(), PlayerEvent{PlayerEventKind::Paused}) > 0; }, 5s));
}

TEST(NetworkOutputTest, AnOpenHomeTakeoverSendsThePlayerNothingMore) {
    FakeRenderer renderer(RendererKind::OpenHome);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    const size_t before = renderer.actions().size();
    renderer.set("source", "3");
    renderer.emitVariable("Product", "SourceIndex", "3");
    ASSERT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DeviceInUse); }));
    output->close();
    EXPECT_TRUE(controlsSince(renderer, before).empty()) << controlsSince(renderer, before).front();
}

// A player on another source, its radio here, moves to its playlist when played to: that is no takeover.
TEST(NetworkOutputTest, AnOpenHomePlayerOnAnotherSourceIsPlayedAtTheFirstTry) {
    FakeRenderer renderer(RendererKind::OpenHome);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    renderer.set("source", "1");
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    ASSERT_TRUE(waitFor([&] { return renderer.subscribers() == 3; }));
    renderer.emitVariable("Product", "SourceIndex", "1"); // as the player reports it once subscribed
    std::this_thread::sleep_for(200ms);
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(raised, kNoError);
    EXPECT_EQ(std::ranges::count(output.events(), PlayerEvent{PlayerEventKind::TakenOver}), 0);
    raised = kNoError;
    renderer.set("source", "3");
    renderer.emitVariable("Product", "SourceIndex", "3");
    EXPECT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DeviceInUse); }));
}

TEST(NetworkOutputTest, AStopAskedForStillStopsThePlayer) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    output->start();
    ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
    const size_t before = renderer.actions().size();
    output->close();
    EXPECT_EQ(controlsSince(renderer, before), std::vector<std::string>{"Stop"});
}

TEST(NetworkOutputTest, AStoppedOutputLeavesThePlayerAlone) {
    for (const auto kind : {RendererKind::UpnpAv, RendererKind::OpenHome}) {
        FakeRenderer renderer(kind);
        renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
        CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
        OutputRun output(renderer, source, k16);
        output->start();
        ASSERT_TRUE(waitFor([&] { return source.rendered() > 0; }));
        output->close();
        const size_t before = renderer.actions().size();
        std::this_thread::sleep_for(2s);
        EXPECT_EQ(renderer.actions().size() - before, 0U) << (kind == RendererKind::OpenHome ? "OpenHome" : "UPnP AV");
        EXPECT_EQ(renderer.subscribers(), 0U) << (kind == RendererKind::OpenHome ? "OpenHome" : "UPnP AV");
    }
}

TEST(NetworkOutputTest, APlayerThatStopsMidTrackIsInUseByAnotherSource) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    std::this_thread::sleep_for(3500ms); // past the moments after Play, when a player may still report its last state
    ASSERT_EQ(raised, kNoError);
    const size_t before = renderer.actions().size();
    renderer.stopOnItsOwn();
    ASSERT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DeviceInUse); }));
    output->close();
    EXPECT_TRUE(controlsSince(renderer, before).empty()) << controlsSince(renderer, before).front();
}

TEST(NetworkOutputTest, TheEndOfTheLastTrackIsNotAStopOfItsOwn) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 4);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.played().size() == 1; }));
    ASSERT_TRUE(waitFor([&] { return renderer.state() == "STOPPED"; }));
    std::this_thread::sleep_for(6s); // past the time a player is given to start
    EXPECT_EQ(raised, kNoError);
}

TEST(NetworkOutputTest, ANextTrackQueuedTooLateIsNotAStopOfItsOwn) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 4, 44100ULL * 2);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.state() == "STOPPED"; }));
    source.queueSecond();
    ASSERT_TRUE(waitFor([&] { return renderer.played().size() == 2; }));
    EXPECT_EQ(raised, kNoError);
}

// A late next track has the time to start that any start has: the stop a player still reports as it begins is no takeover.
TEST(NetworkOutputTest, ALateNextTrackIsGivenTheTimeToStart) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.limitFetch(k16.bytesPerFrame() * 44100ULL);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 6, 44100ULL * 10);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return renderer.state() == "STOPPED"; }, 15s));
    const auto loads = [&] { return std::ranges::count(renderer.actions(), std::string("SetAVTransportURI")); };
    source.queueSecond();
    ASSERT_TRUE(waitFor([&] { return loads() == 2; }));
    renderer.emitLastChange(R"(<TransportState val="STOPPED"/>)"); // its word on the track before, as it starts this one
    std::this_thread::sleep_for(6s);                               // past the time a start is given
    EXPECT_EQ(raised, kNoError);
    EXPECT_TRUE(waitFor([&] { return renderer.played().size() == 2; }, 10s)); // played to its end
    EXPECT_EQ(raised, kNoError);
}

TEST(NetworkOutputTest, APlayerThatDoesNotStartIsReported) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.stayInStandby();
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    ASSERT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DriverError); }));
    const size_t before = renderer.actions().size();
    output->close();
    EXPECT_TRUE(controlsSince(renderer, before).empty()) << controlsSince(renderer, before).front();
}

TEST(NetworkOutputTest, APlayerThatPlaysForAMomentAndStopsIsReported) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.stayInStandby();
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    renderer.emitLastChange(R"(<TransportState val="PLAYING"/>)");
    renderer.emitLastChange(R"(<TransportState val="STOPPED"/>)");
    EXPECT_TRUE(waitFor([&] { return raised == int(audio::ErrorCode::DriverError); }));
}

TEST(NetworkOutputTest, APlayerSlowToStartIsNotReported) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.startSlowly(6000);
    CountingSource source(k16.bytesPerFrame(), 44100ULL * 60);
    OutputRun output(renderer, source, k16);
    std::atomic<int> raised = kNoError;
    output->setErrorHandler([&](const audio::Error& e) { raised = int(e.code()); });
    output->start();
    std::this_thread::sleep_for(6500ms);
    EXPECT_EQ(raised, kNoError);
}

namespace {

// Plays through the whole Server to a fake on the network, found the way a real player is.
class StreamingTest : public ::testing::Test {
protected:
    explicit StreamingTest(RendererKind kind = RendererKind::UpnpAv) : renderer_(kind) {}

    std::unique_ptr<LocalServer> serverFor(Settings settings = {}) {
        settings.autoplay = false;
        auto server = std::make_unique<LocalServer>(settings);
        FinderOptions options;
        options.searchHost = "127.0.0.1";
        options.searchPort = renderer_.ssdpPort();
        options.notifyPort = 0;
        options.searchInterval = 100ms;
        server->setFinderOptions(options);
        Command refresh;
        refresh.kind = CommandKind::RefreshDevices;
        server->submit(std::move(refresh));
        EXPECT_TRUE(waitFor([&] {
            Command again;
            again.kind = CommandKind::RefreshDevices;
            server->submit(std::move(again));
            server->waitIdle();
            return !server->snapshot().devices.network.empty();
        }));
        auto chosen = nativeSettings(server->snapshot().settings);
        selectOutput(chosen, Backend::Network, "uuid:fake-renderer", server->snapshot().devices);
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

    FakeRenderer renderer_;
};

} // namespace

TEST_F(StreamingTest, ATrackReachesThePlayerBitForBit) {
    const auto file = testutil::generatedFile({"network-16", 44100, 16, 2, 1.0});
    const auto reference = audio::testaudio::referenceDecode(file);
    ASSERT_TRUE(reference.error.empty()) << reference.error;
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.received().empty(); })) << server->snapshot().playback.errorDetail;
    const std::string body = renderer_.received().begin()->second;
    ASSERT_EQ(body.size(), 44 + reference.samples.size() * 2);
    for (size_t i = 0; i < reference.samples.size(); ++i) {
        const auto sample = *reinterpret_cast<const int16_t*>(body.data() + 44 + i * 2);
        ASSERT_EQ(int32_t(sample), reference.samples[i]) << i;
    }
    EXPECT_TRUE(server->snapshot().playback.bitPerfect());
    EXPECT_EQ(server->snapshot().playback.activeBackend, Backend::Network);
    server->shutdown();
}

TEST_F(StreamingTest, MatchingTracksAreGaplessOnTwoUrls) {
    const auto first = testutil::generatedFile({"network-gapless-a", 44100, 16, 2, 0.6});
    const auto second = testutil::generatedFile({"network-gapless-b", 44100, 16, 2, 0.4});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return renderer_.played().size() == 2; })) << server->snapshot().playback.errorDetail;
    const auto played = renderer_.played();
    const auto received = renderer_.received();
    EXPECT_EQ(received.at(played[0]).size(), 44 + audio::testaudio::referenceDecode(first).samples.size() * 2);
    EXPECT_EQ(received.at(played[1]).size(), 44 + audio::testaudio::referenceDecode(second).samples.size() * 2);
    EXPECT_TRUE(waitFor([&] { return server->snapshot().queueEntryId == server->snapshot().queue.entries.back().id; }));
    server->shutdown();
}

TEST_F(StreamingTest, ThePositionFollowsAPlayerThatGoesOnReportingTheFirstTrack) {
    const auto first = testutil::generatedFile({"network-lag-a", 44100, 16, 2, 4.0});
    const auto second = testutil::generatedFile({"network-lag-b", 44100, 16, 2, 6.0});
    renderer_.limitFetch(4ULL * 44100);
    renderer_.keepReportingFirstTrack();
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    const uint64_t firstEntry = server->snapshot().queueEntryId;
    uint64_t last = 0;
    bool wentBack = false;
    ASSERT_TRUE(waitFor(
        [&] {
            const auto now = server->snapshot();
            if (now.queueEntryId != firstEntry) return true;
            // The next track's position can reach the snapshot just before its entry does.
            wentBack = wentBack || (now.playback.position + 44100 < last && now.playback.position > 4410);
            last = now.playback.position;
            return false;
        },
        12s));
    EXPECT_FALSE(wentBack);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 1500ms) { // a track's start must not read as a player without a position
        const auto now = server->snapshot();
        EXPECT_LT(now.playback.position, 3ULL * 44100);
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(server->snapshot().playback.error) << server->snapshot().playback.errorDetail;
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Playing);
    server->shutdown();
}

TEST_F(StreamingTest, ThePositionAPlayerKeepsFromBeforeASeekIsNotAChangeOfTrack) {
    const auto first = testutil::generatedFile({"network-stale-a", 44100, 16, 2, 10.0});
    const auto second = testutil::generatedFile({"network-stale-b", 44100, 16, 2, 4.0});
    renderer_.limitFetch(4ULL * 44100);
    renderer_.startSlowly(800);
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    std::this_thread::sleep_for(3500ms); // the fake now reports 2 s or more
    const uint64_t entry = server->snapshot().queueEntryId;
    Command seek;
    seek.kind = CommandKind::Seek;
    seek.frame = uint64_t{6} * 44100;
    server->submit(std::move(seek));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.position >= uint64_t{6} * 44100; }));
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 2500ms) {
        const auto now = server->snapshot();
        EXPECT_EQ(now.queueEntryId, entry);
        EXPECT_GE(now.playback.position + 4410, uint64_t{6} * 44100);
        EXPECT_LT(now.playback.position, uint64_t{9} * 44100);
        std::this_thread::sleep_for(20ms);
    }
    server->shutdown();
}

TEST_F(StreamingTest, ThePositionDoesNotOvertakeAPlayerSlowToReportOne) {
    const auto file = testutil::generatedFile({"network-slow-start", 44100, 16, 2, 20.0});
    renderer_.startSlowly(4500); // reports nothing until then
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 4000ms) {
        EXPECT_LT(server->snapshot().playback.position, uint64_t{2} * 44100);
        std::this_thread::sleep_for(20ms);
    }
    server->shutdown();
}

TEST_F(StreamingTest, TheNextTrackStartsAtItsStartWhenOnlyAnEventNamesIt) {
    const auto first = testutil::generatedFile({"network-event-a", 44100, 16, 2, 4.0});
    const auto second = testutil::generatedFile({"network-event-b", 44100, 16, 2, 6.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    const uint64_t entry = server->snapshot().queueEntryId;
    ASSERT_TRUE(waitFor([&] { return server->snapshot().queueEntryId != entry; }));
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 500ms) {
        EXPECT_LT(server->snapshot().playback.position, uint64_t{2} * 44100);
        std::this_thread::sleep_for(20ms);
    }
    server->shutdown();
}

TEST_F(StreamingTest, SilenceAfterAFormatChangeLeadsTheStreamAndLosesNoFrame) {
    const auto file = testutil::generatedFile({"network-silence", 44100, 16, 2, 1.0});
    const auto reference = audio::testaudio::referenceDecode(file);
    ASSERT_TRUE(reference.error.empty()) << reference.error;
    auto server = serverFor();
    auto settings = nativeSettings(server->snapshot().settings);
    settings.network.timing.formatChangeSilenceMs = 500;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    server->submit(std::move(apply));
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.received().empty(); })) << server->snapshot().playback.errorDetail;
    const std::string body = renderer_.received().begin()->second;
    const size_t silence = 22050;
    ASSERT_EQ(body.size(), 44 + silence * 4 + reference.samples.size() * 2);
    for (size_t i = 44; i < 44 + silence * 4; ++i) ASSERT_EQ(body[i], 0) << i;
    for (size_t i = 0; i < reference.samples.size(); ++i) {
        const auto sample = *reinterpret_cast<const int16_t*>(body.data() + 44 + silence * 4 + i * 2);
        ASSERT_EQ(int32_t(sample), reference.samples[i]) << i;
    }
    server->shutdown();
}

TEST_F(StreamingTest, ASampleRateAboveTheLimitIsConvertedAndShownAsProcessed) {
    const auto file = testutil::generatedFile({"network-limit", 96000, 24, 2, 0.5});
    auto server = serverFor();
    auto settings = nativeSettings(server->snapshot().settings);
    settings.network.maxSampleRate = 48000;
    settings.network.maxBits = 16;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    server->submit(std::move(apply));
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.received().empty(); })) << server->snapshot().playback.errorDetail;
    const auto playback = server->snapshot().playback;
    ASSERT_TRUE(playback.output);
    EXPECT_EQ(playback.output->sampleRate, 48000U);
    EXPECT_EQ(playback.output->type, SampleType::Int16);
    EXPECT_FALSE(playback.bitPerfect());
    server->shutdown();
}

TEST_F(StreamingTest, AChoiceThatChangesNothingKeepsTheStream) {
    const auto file = testutil::generatedFile({"network-keep", 44100, 16, 2, 8.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.currentUri().empty(); }));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.position > 22050; }));
    const auto url = renderer_.currentUri();
    auto settings = nativeSettings(server->snapshot().settings);
    settings.network.maxSampleRate = 96000;
    settings.network.timing.releaseAfterPauseSeconds = 120;
    settings.volume.mode = VolumeMode::Fixed;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    server->submit(std::move(apply));
    server->waitIdle();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(renderer_.currentUri(), url);
    EXPECT_EQ(std::ranges::count(renderer_.actions(), "SetAVTransportURI"), 1);
    server->shutdown();
}

TEST_F(StreamingTest, ThePlayersVolumeIsReadFollowedAndWrittenOnlyWhenAsked) {
    renderer_.setVolume(30, false);
    auto server = serverFor();
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.playerVolume == 30U; }));
    EXPECT_EQ(std::ranges::count(renderer_.actions(), "SetVolume"), 0);
    EXPECT_EQ(std::ranges::count(renderer_.actions(), "SetMute"), 0);

    renderer_.setVolume(65, true); // as read again, or by an event
    ASSERT_TRUE(waitFor([&] {
        renderer_.changeVolume(65, true);
        return server->snapshot().playback.playerVolume == 65U && server->snapshot().playback.playerMuted;
    }));

    Command level;
    level.kind = CommandKind::SetPlayerVolume;
    level.playerVolume = 20;
    server->submit(std::move(level));
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(renderer_.actions(), "SetVolume") == 1; }));
    Command mute;
    mute.kind = CommandKind::SetPlayerMute;
    mute.playerMute = false;
    server->submit(std::move(mute));
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(renderer_.actions(), "SetMute") == 1; }));
    server->shutdown();
}

TEST_F(StreamingTest, FixedVolumeLeavesThePlayerAlone) {
    auto server = serverFor();
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.playerVolume.has_value(); }));
    auto settings = nativeSettings(server->snapshot().settings);
    settings.volume.mode = VolumeMode::Fixed;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    server->submit(std::move(apply));
    EXPECT_TRUE(waitFor([&] { return !server->snapshot().playback.playerVolume; }));
    server->shutdown();
}

TEST_F(StreamingTest, ASeekRestartsTheStreamFromThere) {
    const auto file = testutil::generatedFile({"network-seek", 44100, 16, 2, 4.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.currentUri().empty(); }));
    const auto first = renderer_.currentUri();
    Command seek;
    seek.kind = CommandKind::Seek;
    seek.frame = uint64_t{3} * 44100;
    server->submit(std::move(seek));
    ASSERT_TRUE(waitFor([&] { return renderer_.currentUri() != first; }));
    ASSERT_TRUE(waitFor([&] { return renderer_.received().contains(renderer_.currentUri()); }));
    EXPECT_NEAR(double(renderer_.received().at(renderer_.currentUri()).size()), 44.0 + 44100 * 4, 5000.0);
    server->shutdown();
}

TEST_F(StreamingTest, AManualChangeOfTrackGetsItsOwnUrl) {
    const auto first = testutil::generatedFile({"network-change-a", 44100, 16, 2, 4.0});
    const auto second = testutil::generatedFile({"network-change-b", 44100, 16, 2, 4.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({first, second}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.currentUri().empty(); }));
    const auto url = renderer_.currentUri();
    Command next;
    next.kind = CommandKind::Next;
    server->submit(std::move(next));
    ASSERT_TRUE(waitFor([&] { return renderer_.currentUri() != url; }));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    EXPECT_TRUE(waitFor([&] {
        const auto snapshot = server->snapshot();
        return snapshot.queueEntryId == snapshot.queue.entries.back().id;
    }));
    server->shutdown();
}

TEST_F(StreamingTest, ADspChangeRestartsTheStreamFromTheAudiblePosition) {
    const auto file = testutil::generatedFile({"network-dsp", 44100, 16, 2, 6.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.currentUri().empty(); }));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.position > 44100; }));
    EXPECT_LT(server->snapshot().playback.position, 44100ULL * 3); // not what has been sent, about 5 s
    const auto url = renderer_.currentUri();
    const auto position = server->snapshot().playback.position;
    auto settings = nativeSettings(server->snapshot().settings);
    settings.eq.enabled = true;
    settings.eq.bands.front().gainDb = 3.0;
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(settings);
    server->submit(std::move(apply));
    ASSERT_TRUE(waitFor([&] { return renderer_.currentUri() != url; }));
    EXPECT_NEAR(double(server->snapshot().playback.position), double(position), 44100.0);
    EXPECT_TRUE(waitFor([&] { return !server->snapshot().playback.bitPerfect(); }));
    server->shutdown();
}

TEST_F(StreamingTest, ThePositionFollowsWhatThePlayerHasPlayedNotWhatWasSent) {
    const auto file = testutil::generatedFile({"network-position", 44100, 16, 2, 10.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    std::this_thread::sleep_for(1500ms);
    EXPECT_LT(server->snapshot().playback.position, 44100ULL * 3); // about 5 s were sent by now
    server->shutdown();
}

TEST_F(StreamingTest, ThePlayersOwnPauseAndPlayAreFollowed) {
    const auto file = testutil::generatedFile({"network-remote", 44100, 16, 2, 8.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    renderer_.emitLastChange(R"(<TransportState val="PLAYING"/>)");
    std::this_thread::sleep_for(200ms);
    renderer_.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Paused; }));
    renderer_.emitLastChange(R"(<TransportState val="PLAYING"/>)");
    EXPECT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    server->shutdown();
}

// What the player reports while CamelPlayer closes is dropped; it does not end the program.
TEST_F(StreamingTest, ThePlayersReportsWhileClosingAreDropped) {
    const auto file = testutil::generatedFile({"network-closing", 44100, 16, 2, 8.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    std::atomic<bool> closed = false;
    std::thread reports([&] {
        for (bool paused = true; !closed; paused = !paused) {
            renderer_.emitLastChange(paused ? R"(<TransportState val="PAUSED_PLAYBACK"/>)"
                                            : R"(<TransportState val="PLAYING"/>)");
            std::this_thread::sleep_for(2ms);
        }
    });
    std::this_thread::sleep_for(100ms);
    server->shutdown();
    closed = true;
    reports.join();
}

TEST_F(StreamingTest, ATakeoverStopsWithItsReason) {
    const auto file = testutil::generatedFile({"network-takeover", 44100, 16, 2, 8.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    renderer_.takeOver("http://other-app/song");
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.error.has_value(); }));
    EXPECT_NE(server->snapshot().playback.errorDetail.find("in use by another source"), std::string::npos);
    EXPECT_NE(server->snapshot().playback.state, PlaybackState::Playing);
    server->shutdown();
}

TEST_F(StreamingTest, APlayerThatDisappearsPausesAsNotConnected) {
    const auto file = testutil::generatedFile({"network-gone", 44100, 16, 2, 8.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    renderer_.disappear();
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.error.has_value(); }, 20s));
    EXPECT_NE(server->snapshot().playback.errorDetail.find("not connected"), std::string::npos)
        << server->snapshot().playback.errorDetail;
    EXPECT_EQ(server->snapshot().settings.backend, Backend::Network);
    server->shutdown();
}
