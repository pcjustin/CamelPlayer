// Saved devices and output parameters (docs/Plan.md §2, M2-R10): a missing device fails playback with a notice and is
// never replaced, it plays again once it returns, and parameters the device could not take reach the status area
// while the saved settings stay as requested.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "Common/util/Text.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

const char* kAdjustment = "ASIO buffer: requested 100, using 512 (driver min 64 / max 2048 / granularity -1)";

// A capture device that is unplugged until `connected`, and reports one parameter it replaced.
class PluggableOutput final : public IOutput {
public:
    PluggableOutput(std::unique_ptr<IOutput> inner, std::atomic<bool>& connected)
        : inner_(std::move(inner)), connected_(connected) {}
    bool supports(const DeviceFormat& f) override {
        requireConnected();
        return inner_->supports(f);
    }
    void open(const DeviceFormat& f, IRenderSource* source) override {
        requireConnected();
        inner_->open(f, source);
    }
    void start() override { inner_->start(); }
    void pause() override { inner_->pause(); }
    void close() override { inner_->close(); }
    OutputStatus status() const override {
        OutputStatus s = inner_->status();
        s.adjustments = {kAdjustment};
        return s;
    }

private:
    void requireConnected() const {
        if (!connected_) throw Error(ErrorCode::DeviceNotFound, "test device not connected");
    }
    std::unique_ptr<IOutput> inner_;
    std::atomic<bool>& connected_;
};

Command open(const char* name) {
    Command c;
    c.kind = CommandKind::OpenAndPlay;
    c.source.emplace(toUtf8(testutil::testFile(name).wstring()), FrameRange{}, std::nullopt);
    return c;
}

} // namespace

TEST(DeviceIdentity, MissingDeviceFailsWithANoticeAndPlaysWhenItReturns) {
    std::atomic<bool> connected{false};
    CaptureOptions options;
    options.file = testutil::tempDir() / "device-identity.raw";
    options.realtime = true;
    Settings settings;
    settings.wasapi.deviceId = "{saved-endpoint}";
    LocalServer server(settings, {}, [&](const Settings&) {
        return std::make_unique<PluggableOutput>(createCaptureOutput(options), connected);
    });
    server.submit(open("16-44.1k"));
    server.waitIdle();
    auto s = server.snapshot();
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    ASSERT_TRUE(s.playback.error);
    EXPECT_EQ(*s.playback.error, ErrorCode::DeviceNotFound);
    EXPECT_NE(s.playback.errorDetail.find("not connected"), std::string::npos) << s.playback.errorDetail;
    EXPECT_FALSE(s.playback.activeBackend);
    EXPECT_EQ(s.settings.wasapi.deviceId, "{saved-endpoint}"); // not replaced by another device

    connected = true;
    Command play;
    play.kind = CommandKind::Play;
    server.submit(play);
    server.waitIdle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        s = server.snapshot();
        if (s.playback.state == PlaybackState::Playing && !s.playback.outputAdjustments.empty()) break;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    ASSERT_EQ(s.playback.outputAdjustments.size(), 1u);
    EXPECT_EQ(s.playback.outputAdjustments.front(), kAdjustment);
    EXPECT_EQ(s.settings.wasapi.deviceId, "{saved-endpoint}");
    Command stop;
    stop.kind = CommandKind::Stop;
    server.submit(stop);
    server.waitIdle();
}
