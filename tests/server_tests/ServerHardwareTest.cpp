#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <tuple>

#include "Server/Playback/LocalServer.h"
#include "Common/util/Text.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

struct Route {
    Backend backend;
    const char* name;
};
constexpr Route routes[] = {{Backend::Wasapi, "Chord"},
                            {Backend::Asio, "Chord"},
                            {Backend::Ks, "Chord"},
                            {Backend::Wasapi, "Cirrus"},
                            {Backend::Ks, "Cirrus"}};

class ServerHardware : public ::testing::TestWithParam<std::tuple<int, audio::DecoderKind>> {};

TEST_P(ServerHardware, DISABLED_PlayPauseSeekStop) {
    const auto [routeIndex, decoder] = GetParam();
    const auto route = routes[routeIndex];
    // Use an ephemeral Server so hardware tests never modify the user's settings.
    auto server = std::make_shared<LocalServer>(Settings{});
    auto send = [&](Command c) {
        server->submit(std::move(c));
        server->waitIdle();
        return server->snapshot();
    };
    Command c;
    c.kind = CommandKind::RefreshDevices;
    auto s = send(c);
    ASSERT_FALSE(s.commandError);
    const auto& devices = route.backend == Backend::Wasapi ? s.devices.wasapi
                          : route.backend == Backend::Asio ? s.devices.asio
                                                           : s.devices.ks;
    const auto device = std::find_if(devices.begin(), devices.end(), [&](const DeviceInfo& d) {
        return d.name.find(route.name) != std::string::npos && d.name.find("unavailable") == std::string::npos;
    });
    ASSERT_NE(device, devices.end()) << route.name;
    c = {};
    c.kind = CommandKind::ApplySettings;
    c.settings = s.settings;
    c.settings->backend = route.backend;
    c.settings->decoder = decoder;
    if (route.backend == Backend::Wasapi)
        c.settings->wasapi.deviceId = device->id;
    else if (route.backend == Backend::Asio)
        c.settings->asio.driverClsid = device->id;
    else
        c.settings->ks.deviceId = device->id;
    send(c);
    c = {};
    c.kind = CommandKind::OpenAndPlay;
    const auto file = testutil::tempDir().parent_path().parent_path() / "testdata" / "Sample_BeeMoved_96kHz24bit.flac";
    ASSERT_TRUE(std::filesystem::exists(file));
    c.source.emplace(toUtf8(file.wstring()));
    s = send(c);
    ASSERT_FALSE(s.playback.error) << s.playback.errorDetail;
    ASSERT_EQ(s.playback.state, PlaybackState::Playing);
    ASSERT_TRUE(s.playback.source);
    ASSERT_TRUE(s.playback.output);
    EXPECT_EQ(s.playback.backend, route.backend);
    EXPECT_EQ(s.playback.decoder, decoder);
    EXPECT_EQ(s.playback.source->sampleRate, s.playback.output->sampleRate);
    const auto before = s.playback.position;
    std::this_thread::sleep_for(350ms);
    EXPECT_GT(server->snapshot().playback.position, before);
    c = {};
    c.kind = CommandKind::Pause;
    s = send(c);
    ASSERT_EQ(s.playback.state, PlaybackState::Paused);
    std::this_thread::sleep_for(150ms);
    const auto position = server->snapshot().playback.position;
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(server->snapshot().playback.position, position);
    c = {};
    c.kind = CommandKind::Seek;
    c.frame = 10ull * s.playback.source->sampleRate;
    s = send(c);
    EXPECT_EQ(s.playback.position, c.frame);
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    c = {};
    c.kind = CommandKind::Play;
    s = send(c);
    ASSERT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    EXPECT_EQ(s.playback.underruns, 0u);
    s = send(Command{});
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    EXPECT_EQ(s.playback.position, 0u);
    EXPECT_FALSE(s.playback.output);
}

std::string hardwareCaseName(const ::testing::TestParamInfo<ServerHardware::ParamType>& info) {
    const auto [index, decoder] = info.param;
    return std::string(routes[index].name) + "_" + backendId(routes[index].backend) + "_" + audio::decoderId(decoder);
}

INSTANTIATE_TEST_SUITE_P(ServerRoutes, ServerHardware,
                         ::testing::Combine(::testing::Values(0, 1, 2, 3, 4), ::testing::ValuesIn(audio::kAllDecoders)),
                         hardwareCaseName);

} // namespace
