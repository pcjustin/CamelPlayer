// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Discord Rich Presence (docs/Plan.md §8.4) against a stand-in Discord pipe, a stand-in cloudflared and a fake web server.
#include "FakeHttp.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Common/util/Text.h"
#include "Server/Integration/CoverTunnel.h"
#include "Server/Integration/DiscordPresence.h"
#include "Server/Integration/DiscordRpc.h"
#include "Server/Support/Http.h"
#include "Server/Support/Updates.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using json = nlohmann::json;

namespace {

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

std::wstring pipePrefix(const char* test) {
    return L"\\\\.\\pipe\\camel-test-discord-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::wstring(test, test + std::strlen(test)) + L"-";
}

// One Discord session on <prefix>0: READY after the handshake, and an answer to every command with its nonce.
class FakeDiscord {
public:
    explicit FakeDiscord(std::wstring prefix) : name_(std::move(prefix) + L"0") {
        pipe_ = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
                                 65536, 0, nullptr);
        EXPECT_NE(pipe_, INVALID_HANDLE_VALUE);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeDiscord() {
        // A client that never came would leave ConnectNamedPipe waiting.
        if (!connected_) {
            const HANDLE unblock =
                CreateFileW(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (unblock != INVALID_HANDLE_VALUE) CloseHandle(unblock);
        }
        thread_.join();
        CloseHandle(pipe_);
    }
    FakeDiscord(const FakeDiscord&) = delete;
    FakeDiscord& operator=(const FakeDiscord&) = delete;

    std::vector<std::pair<uint32_t, json>> frames() const {
        std::lock_guard lock(mutex_);
        return frames_;
    }
    // The activities of the SET_ACTIVITY commands so far; null for a cleared one.
    std::vector<json> activities() const {
        std::vector<json> result;
        for (const auto& [op, body] : frames())
            if (op == 1 && body.value("cmd", "") == "SET_ACTIVITY")
                result.push_back(body["args"].contains("activity") ? body["args"]["activity"] : json());
        return result;
    }

private:
    void serve() {
        if (!ConnectNamedPipe(pipe_, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        connected_ = true;
        for (;;) {
            uint32_t header[2]{};
            if (!read(reinterpret_cast<char*>(header), sizeof(header))) break;
            std::string payload(header[1], '\0');
            if (!read(payload.data(), header[1])) break;
            const auto body = json::parse(payload, nullptr, false);
            {
                std::lock_guard lock(mutex_);
                frames_.emplace_back(header[0], body);
            }
            if (header[0] == 0)
                reply({{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", {{"v", 1}}}});
            else if (header[0] == 1)
                reply({{"cmd", body["cmd"]}, {"evt", nullptr}, {"nonce", body["nonce"]}, {"data", json::object()}});
        }
        DisconnectNamedPipe(pipe_);
    }
    bool read(char* data, DWORD size) {
        while (size > 0) {
            DWORD got = 0;
            if (!ReadFile(pipe_, data, size, &got, nullptr) || got == 0) return false;
            data += got;
            size -= got;
        }
        return true;
    }
    void reply(const json& body) {
        const std::string payload = body.dump();
        uint32_t header[2] = {1, uint32_t(payload.size())};
        std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
        frame += payload;
        DWORD written = 0;
        WriteFile(pipe_, frame.data(), DWORD(frame.size()), &written, nullptr);
    }

    std::wstring name_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    std::vector<std::pair<uint32_t, json>> frames_;
    std::thread thread_;
};

DiscordClient::Options fastOptions(const char* test) {
    DiscordClient::Options options;
    options.pipePrefix = pipePrefix(test);
    options.clientId = "test-client";
    options.reconnect = std::chrono::milliseconds(100);
    options.retry = std::chrono::milliseconds(100);
    options.merge = std::chrono::milliseconds(10);
    options.timeout = std::chrono::milliseconds(1000);
    return options;
}

DiscordActivity sampleActivity() {
    DiscordActivity activity;
    activity.title = "Welcome Back to Night City";
    activity.artists = "Tsuneo Imahori";
    activity.album = "Cyberpunk: Edgerunners 2";
    activity.smallImage = "bitperfect";
    activity.smallText = "FLAC 24-bit 96 kHz · Bit-perfect";
    activity.startMs = 1'000'000;
    activity.endMs = 1'146'000;
    return activity;
}

std::filesystem::path systemFile(const wchar_t* name) {
    wchar_t folder[MAX_PATH]{};
    GetSystemDirectoryW(folder, MAX_PATH);
    return std::filesystem::path(folder) / name;
}

} // namespace

TEST(DiscordPresence, TextsStayWithinDiscordsLimits) {
    EXPECT_EQ(discordText(""), "");
    EXPECT_EQ(discordText("A"), "A ");
    EXPECT_EQ(discordText("Ab"), "Ab");
    EXPECT_EQ(toWide(discordText(std::string(200, 'x'))).size(), 128u);
    // A character outside the BMP is never cut in half.
    std::string emoji;
    for (int i = 0; i < 70; ++i) emoji += "\xF0\x9F\x8E\xB5";
    const auto cut = toWide(discordText(emoji));
    EXPECT_EQ(cut.size(), 128u);
    EXPECT_FALSE(IS_HIGH_SURROGATE(cut.back()));
}

TEST(DiscordPresence, ActivityListensWithTheTitleInTheFriendsList) {
    const auto activity = json::parse(discordActivityJson(sampleActivity()));
    EXPECT_EQ(activity["type"], 2);
    EXPECT_EQ(activity["status_display_type"], 2);
    EXPECT_EQ(activity["details"], "Welcome Back to Night City");
    EXPECT_EQ(activity["state"], "Tsuneo Imahori");
    EXPECT_EQ(activity["timestamps"]["start"], 1'000'000);
    EXPECT_EQ(activity["timestamps"]["end"], 1'146'000);
    EXPECT_EQ(activity["assets"]["large_image"], "camelplayer");
    EXPECT_EQ(activity["assets"]["large_text"], "Cyberpunk: Edgerunners 2");
    EXPECT_EQ(activity["assets"]["small_image"], "bitperfect");
    EXPECT_EQ(activity["assets"]["small_text"], "FLAC 24-bit 96 kHz · Bit-perfect");
    EXPECT_FALSE(activity.contains("buttons"));

    auto withCover = sampleActivity();
    withCover.largeImage = "https://a-b.trycloudflare.com/k.jpg";
    withCover.endMs = 0;
    const auto covered = json::parse(discordActivityJson(withCover, false));
    EXPECT_EQ(covered["assets"]["large_image"], "https://a-b.trycloudflare.com/k.jpg");
    EXPECT_FALSE(covered["timestamps"].contains("end"));
    EXPECT_FALSE(covered.contains("status_display_type"));
}

TEST(DiscordPresence, OnlyChangesAndSeeksNeedAnUpdate) {
    const auto shown = sampleActivity();
    auto drifted = shown;
    drifted.startMs += 3000;
    drifted.endMs += 3000;
    EXPECT_FALSE(discordActivityChanged(shown, drifted));
    auto seeked = shown;
    seeked.startMs -= 30'000;
    seeked.endMs -= 30'000;
    EXPECT_TRUE(discordActivityChanged(shown, seeked));
    auto other = shown;
    other.title = "Another";
    EXPECT_TRUE(discordActivityChanged(shown, other));
    auto covered = shown;
    covered.largeImage = "https://x.trycloudflare.com/k.jpg";
    EXPECT_TRUE(discordActivityChanged(shown, covered));
    EXPECT_TRUE(discordActivityChanged(shown, std::nullopt));
    EXPECT_FALSE(discordActivityChanged(std::nullopt, std::nullopt));
}

TEST(DiscordPresence, ActivityFollowsThePlayingEntryAndPathState) {
    Snapshot snapshot;
    snapshot.queueEntryId = 7;
    QueueEntry entry{7, TrackSource("C:/music/Boat.flac")};
    entry.title = "Boat";
    entry.artist = "Ed Sheeran";
    entry.album = "- (Deluxe)";
    snapshot.queue.entries = QueueEntries{entry};
    auto& playback = snapshot.playback;
    playback.state = PlaybackState::Playing;
    playback.pathUtf8 = "C:/music/Boat.flac";
    playback.format = audio::AudioFormat::Flac;
    playback.source = audio::SourceInfo{96000, 2, 24, 0, std::nullopt};
    playback.position = 96000 * 10;
    playback.total = 96000 * 185;
    playback.plan = audio::OutputPlan{};
    DiscordTexts texts;
    texts.bitPerfect = "Bit-perfect";

    const auto activity = discordActivity(snapshot, texts, 1'000'000);
    ASSERT_TRUE(activity);
    EXPECT_EQ(activity->title, "Boat");
    EXPECT_EQ(activity->artists, "Ed Sheeran");
    EXPECT_EQ(activity->album, "- (Deluxe)");
    EXPECT_EQ(activity->startMs, 990'000);
    EXPECT_EQ(activity->endMs, 1'175'000);
    EXPECT_EQ(activity->smallImage, "bitperfect");
    EXPECT_EQ(activity->smallText, "FLAC 24-bit 96 kHz · Bit-perfect");

    playback.source->sampleRate = 44100;
    playback.plan->state = audio::PipelineState::Processed;
    texts.processed = "已處理";
    EXPECT_EQ(discordActivity(snapshot, texts, 0)->smallText, "FLAC 24-bit 44.1 kHz · 已處理");
    // A file outside the queue's metadata shows its name.
    snapshot.queueEntryId = 8;
    EXPECT_EQ(discordActivity(snapshot, texts, 0)->title, "Boat");
    playback.state = PlaybackState::Paused;
    EXPECT_FALSE(discordActivity(snapshot, texts, 0));
}

TEST(DiscordPresence, ClientHandshakesShowsAndClears) {
    FakeDiscord discord(pipePrefix("show"));
    DiscordClient client(fastOptions("show"));
    client.setEnabled(true);
    ASSERT_TRUE(waitFor([&] { return client.state() == DiscordClient::State::Connected; }));
    client.show(sampleActivity());
    ASSERT_TRUE(waitFor([&] {
        const auto activities = discord.activities();
        return !activities.empty() && activities.back().is_object();
    }));
    const auto frames = discord.frames();
    EXPECT_EQ(frames.front().first, 0u);
    EXPECT_EQ(frames.front().second["client_id"], "test-client");
    EXPECT_EQ(discord.activities().back()["details"], "Welcome Back to Night City");
    // Position updates within a seek's tolerance send nothing more.
    const auto sent = discord.activities().size();
    auto drifted = sampleActivity();
    drifted.startMs += 1000;
    client.show(drifted);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(discord.activities().size(), sent);
    client.show(std::nullopt);
    ASSERT_TRUE(waitFor([&] { return discord.activities().back().is_null(); }));
    client.setEnabled(false);
    EXPECT_TRUE(waitFor([&] { return client.state() == DiscordClient::State::Off; }));
}

TEST(DiscordPresence, ClientWaitsForDiscordToStart) {
    DiscordClient client(fastOptions("late"));
    client.setEnabled(true);
    client.show(sampleActivity());
    ASSERT_TRUE(waitFor([&] { return client.state() == DiscordClient::State::Searching; }));
    FakeDiscord discord(pipePrefix("late"));
    ASSERT_TRUE(waitFor([&] { return client.state() == DiscordClient::State::Connected; }));
    EXPECT_TRUE(waitFor([&] {
        const auto activities = discord.activities();
        return !activities.empty() && activities.back().is_object();
    }));
    client.setEnabled(false);
    EXPECT_TRUE(waitFor([&] { return client.state() == DiscordClient::State::Off; }));
}

TEST(CoverTunnel, FindsTheQuickTunnelInCloudflaredsOutput) {
    EXPECT_EQ(quickTunnelUrl("2026-11-01T05:00:00Z INF |  https://calm-lake-bird.trycloudflare.com  |"),
              "https://calm-lake-bird.trycloudflare.com");
    EXPECT_FALSE(quickTunnelUrl("Requesting new quick Tunnel on trycloudflare.com..."));
    EXPECT_FALSE(quickTunnelUrl("POST https://api.trycloudflare.com/tunnel"));
}

TEST(CoverTunnel, ServesTheCoverWhilePlayingAndStopsWhenIdle) {
    const auto cmd = systemFile(L"cmd.exe");
    std::atomic<uint16_t> port{0};
    CoverTunnel::Options options;
    options.executable = cmd;
    options.sha256 = update::sha256File(cmd);
    options.idle = std::chrono::milliseconds(500);
    options.command = [&](const std::filesystem::path& executable, uint16_t local) {
        port = local;
        return L"\"" + executable.wstring() +
               L"\" /c \"echo Your quick Tunnel: https://test-tunnel.trycloudflare.com && ping -n 60 127.0.0.1 > nul\"";
    };
    CoverTunnel tunnel(options);
    const std::vector<uint8_t> cover = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0xFF, 0xD9};
    tunnel.serve(cover);
    tunnel.setEnabled(true);
    EXPECT_EQ(tunnel.url(), "");
    tunnel.setPlaying(true);
    ASSERT_TRUE(waitFor([&] { return tunnel.status().state == CoverTunnel::State::Ready; }));
    const auto url = tunnel.url();
    ASSERT_TRUE(url.starts_with("https://test-tunnel.trycloudflare.com/"));
    ASSERT_TRUE(url.ends_with(".jpg"));
    const auto path = url.substr(std::string("https://test-tunnel.trycloudflare.com").size());
    const auto local = "http://127.0.0.1:" + std::to_string(port.load());
    const auto served = http::get(local + path, "image/jpeg", {}, 1 << 20);
    EXPECT_EQ(served.status, 200u);
    EXPECT_EQ(std::vector<uint8_t>(served.body.begin(), served.body.end()), cover);
    EXPECT_EQ(http::get(local + "/other.jpg", "image/jpeg", {}, 1 << 20).status, 404u);
    tunnel.setPlaying(false);
    EXPECT_TRUE(waitFor([&] { return tunnel.status().state == CoverTunnel::State::Off && tunnel.url().empty(); }));
}

TEST(CoverTunnel, DownloadsCloudflaredOnlyWhenItsChecksumMatches) {
    testutil::FakeHttp web;
    const std::string binary = "not really cloudflared";
    web.route("/cloudflared.exe", binary);
    const auto folder = testutil::tempDir() / "cover-tunnel-download";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto reference = folder / "reference";
    std::ofstream(reference, std::ios::binary) << binary;

    CoverTunnel::Options wrong;
    wrong.executable = folder / "tools" / "cloudflared.exe";
    wrong.downloadUrl = web.base() + "/cloudflared.exe";
    wrong.sha256 = std::string(64, '0');
    {
        CoverTunnel tunnel(wrong);
        tunnel.setEnabled(true);
        tunnel.setPlaying(true);
        ASSERT_TRUE(waitFor([&] { return tunnel.status().state == CoverTunnel::State::Failed; }));
        EXPECT_NE(tunnel.status().error.find("SHA-256"), std::string::npos);
    }
    EXPECT_FALSE(std::filesystem::exists(wrong.executable));

    auto right = wrong;
    right.sha256 = update::sha256File(reference);
    {
        CoverTunnel tunnel(right);
        tunnel.setEnabled(true);
        tunnel.setPlaying(true);
        // The download lands and is checked; the stand-in is no program, so starting it then fails.
        ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(right.executable); }));
        ASSERT_TRUE(waitFor([&] { return tunnel.status().state == CoverTunnel::State::Failed; }));
        EXPECT_NE(tunnel.status().error.find("did not start"), std::string::npos);
    }
    EXPECT_EQ(update::sha256File(right.executable), right.sha256);
}
