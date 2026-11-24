// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// API versions and Server updates from a Control point (docs/Plan.md §17.8).
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "Common/util/Text.h"
#include "FakeHttp.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Remote/RemoteAccess.h"
#include "Server/Remote/RemoteServer.h"
#include "Server/Support/Updates.h"
#include "Server/Support/ZipWriter.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Points the update check at a fake GitHub and the downloads into the test's folder.
class Redirected {
public:
    Redirected(const testutil::FakeHttp& github, const fs::path& temp) {
        const std::string base = github.base();
        SetEnvironmentVariableW(L"CAMELPLAYER_UPDATE_URL", std::wstring(base.begin(), base.end()).c_str());
        GetEnvironmentVariableW(L"TMP", oldTemp_, DWORD(std::size(oldTemp_)));
        fs::create_directories(temp);
        SetEnvironmentVariableW(L"TMP", (temp.wstring() + L"\\").c_str());
    }
    ~Redirected() {
        SetEnvironmentVariableW(L"CAMELPLAYER_UPDATE_URL", nullptr);
        SetEnvironmentVariableW(L"TMP", oldTemp_);
    }
    Redirected(const Redirected&) = delete;
    Redirected& operator=(const Redirected&) = delete;

private:
    wchar_t oldTemp_[MAX_PATH] = {};
};

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

class ServerUpdateTest : public ::testing::Test {
protected:
    fs::path dir;
    std::unique_ptr<LocalServer> local;
    std::unique_ptr<RemoteAccess> access;
    std::vector<std::unique_ptr<RemoteServer>> points;
    testutil::FakeHttp github;
    std::optional<Redirected> redirected;
    std::mutex mutex;
    std::optional<std::pair<update::Release, update::Prepared>> installed;

    void SetUp() override {
        dir = testutil::tempDir() / "server-update" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(dir / "data");
        redirected.emplace(github, dir / "temp");
    }

    void TearDown() override {
        points.clear();
        if (access) access->stop();
        access.reset();
        if (local) local->shutdown();
        local.reset();
        redirected.reset();
    }

    void start(const fs::path& programDirectory) {
        Settings s;
        s.autoplay = false;
        s.allowControl = true;
        s.wasapi.deviceId = "zone-1";
        SavedOutput output;
        output.backend = Backend::Wasapi;
        output.id = output.wasapi.deviceId = "zone-1";
        output.name = "Zone 1";
        output.enabled = true;
        output.zone.autoplay = false;
        s.outputs.push_back(output);
        local = std::make_unique<LocalServer>(fromJson(toJson(s)), fs::path{}, [this](const Settings&) {
            audio::CaptureOptions options;
            options.file = dir / "zone.raw";
            options.accepts = [](const audio::DeviceFormat& f) { return f.type == audio::SampleType::Int32; };
            options.realtime = true;
            return audio::createCaptureOutput(options);
        });
        RemoteAccess::Options options;
        options.data = dir / "data";
        options.name = "Test PC";
        options.port = 0;
        options.loopbackOnly = true;
        options.announce = false;
        options.appVersion = "0.6.0";
        options.programDirectory = programDirectory;
        access = std::make_unique<RemoteAccess>(*local, options);
        access->setInstall([this](const update::Release& release, const update::Prepared& prepared) {
            std::lock_guard lock(mutex);
            installed.emplace(release, prepared);
        });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!access->port() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
        ASSERT_TRUE(access->port());
    }

    RemoteServer& connect() {
        const auto name = "Control " + std::to_string(points.size() + 1);
        const auto port = *access->port();
        const auto paired = RemoteServer::pair("127.0.0.1", port, name, points.empty() ? "" : access->pairingCode());
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = port;
        options.cache = dir / "cache";
        options.token = paired.token;
        points.push_back(std::make_unique<RemoteServer>(options));
        return *points.back();
    }

    // A release `version` on the fake GitHub, its zip and sums checking out.
    void publish(const std::string& version) {
        const auto base = github.base();
        github.route("/repos/pcjustin/CamelPlayer/releases/latest",
                     R"({"tag_name":"v)" + version + R"(","draft":false,"prerelease":false,"body":"Notes","assets":[)" +
                         R"({"name":"CamelPlayer-)" + version + R"(-win-x64.zip","browser_download_url":")" + base +
                         R"(/zip"},{"name":"CamelPlayer-)" + version + R"(-SHA256SUMS.txt","browser_download_url":")" + base +
                         R"(/sums"}]})");
        const auto zipFile = dir / "release.zip";
        {
            ZipWriter zip(zipFile);
            zip.add("CamelPlayer.exe", "new");
            zip.add("install-manifest.txt", "the list");
            zip.finish();
        }
        github.route("/zip", readText(zipFile));
        github.route("/sums", update::sha256File(zipFile) + "  CamelPlayer-" + version + "-win-x64.zip\n");
    }
};

} // namespace

// A Control point inside the Server's range of API versions connects; outside it, it is told which side needs an
// update.
TEST(ApiVersionTest, AControlPointOutsideTheServersVersionsIsToldWhichSideToUpdate) {
    const auto dir = testutil::tempDir() / ("api-versions-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    LocalServer local(Settings{}, fs::path{});
    ApiServer::Options served;
    served.port = 0;
    served.loopbackOnly = true;
    served.apiMin = 2;
    served.apiMax = 3;
    served.appVersion = "0.7.0";
    ApiServer api(local, served);
    const auto paired = RemoteServer::pair("127.0.0.1", api.port(), "Control", "");
    const auto open = [&](uint32_t version) {
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = api.port();
        options.cache = dir / "cache";
        options.token = paired.token;
        options.apiVersion = version;
        return std::make_unique<RemoteServer>(options);
    };
    for (const uint32_t inside : {2U, 3U}) {
        const auto point = open(inside);
        EXPECT_EQ(point->link(), IServer::Link::Connected);
        EXPECT_EQ(point->serverVersion(), "0.7.0");
    }
    try {
        open(4);
        ADD_FAILURE() << "a newer Control point connected";
    } catch (const VersionMismatch& e) {
        EXPECT_TRUE(e.serverOlder);
        EXPECT_EQ(e.serverVersion, "0.7.0");
    }
    try {
        open(1);
        ADD_FAILURE() << "an older Control point connected";
    } catch (const VersionMismatch& e) {
        EXPECT_FALSE(e.serverOlder);
    }
    local.shutdown();
}

// Update Server: the Server finds the release itself and says what stops, the zones playing and the other Control
// points, then installs it, verified, through its own way out.
TEST_F(ServerUpdateTest, AControlPointHasTheServerFindAndInstallTheRelease) {
    start(dir / "program");
    fs::create_directories(dir / "program");
    auto& asking = connect();
    connect();
    publish("0.6.1");
    Command play;
    play.kind = CommandKind::QueueReplace;
    play.sources.emplace_back(toUtf8(testutil::testFile("16-44.1k").wstring()));
    local->submit(play);
    local->waitIdle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (local->snapshot().playback.state != PlaybackState::Playing && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);

    const auto state = asking.serverUpdate();
    EXPECT_EQ(state["version"], "0.6.0");
    EXPECT_EQ(state["release"]["version"], "0.6.1");
    EXPECT_EQ(state["administrator"], false);
    EXPECT_EQ(state["playing"], nlohmann::json::array({"Zone 1"}));
    EXPECT_EQ(state["connected"], nlohmann::json::array({"Control 2"})) << "the others, not the one asking";

    EXPECT_EQ(asking.updateServer(), "0.6.1");
    const auto until = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        {
            std::lock_guard lock(mutex);
            if (installed || std::chrono::steady_clock::now() > until) break;
        }
        std::this_thread::sleep_for(20ms);
    }
    std::lock_guard lock(mutex);
    ASSERT_TRUE(installed);
    EXPECT_EQ(installed->first.version, "0.6.1");
    EXPECT_EQ(readText(installed->second.files / "CamelPlayer.exe"), "new") << "downloaded and checked on the Server";
}

TEST_F(ServerUpdateTest, AServerUpToDateHasNothingToInstall) {
    start(dir / "program");
    auto& asking = connect();
    publish("0.6.0");
    EXPECT_TRUE(asking.serverUpdate()["release"].is_null());
    EXPECT_THROW(asking.updateServer(), std::invalid_argument);
    std::lock_guard lock(mutex);
    EXPECT_FALSE(installed);
}

// A Server in a folder only an administrator may change is updated at its own computer.
TEST_F(ServerUpdateTest, AServerThatNeedsAnAdministratorIsNotUpdatedFromAfar) {
    if (elevated()) GTEST_SKIP() << "this process may change any folder";
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    start(windows);
    auto& asking = connect();
    publish("0.6.1");
    const auto state = asking.serverUpdate();
    EXPECT_EQ(state["administrator"], true);
    EXPECT_EQ(state["release"]["version"], "0.6.1");
    EXPECT_THROW(asking.updateServer(), std::logic_error);
    std::lock_guard lock(mutex);
    EXPECT_FALSE(installed);
}
