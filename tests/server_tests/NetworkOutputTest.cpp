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
        if (!second_ || !queued_) return std::nullopt;
        return audio::StreamTrack{L"second.flac", second_, 0, 2};
    }
    void queueSecond() { queued_ = true; }
    uint64_t rendered() const { return at_.load(); }

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

TEST(NetworkOutputTest, AnOpenHomePlayerGetsTheQueueAsItsPlaylist) {
    FakeRenderer renderer(RendererKind::OpenHome);
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
    EXPECT_EQ(server->snapshot().queueEntryId, server->snapshot().queue.entries.back().id);
    server->shutdown();
}

TEST_F(StreamingTest, ADspChangeRestartsTheStreamFromTheAudiblePosition) {
    const auto file = testutil::generatedFile({"network-dsp", 44100, 16, 2, 6.0});
    renderer_.limitFetch(4ULL * 44100);
    auto server = serverFor();
    server->submit(queue({file}));
    ASSERT_TRUE(waitFor([&] { return !renderer_.currentUri().empty(); }));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.position > 44100; }));
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
    EXPECT_FALSE(server->snapshot().playback.bitPerfect());
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
    EXPECT_NE(server->snapshot().playback.errorDetail.find("not connected"), std::string::npos);
    EXPECT_EQ(server->snapshot().settings.backend, Backend::Network);
    server->shutdown();
}
