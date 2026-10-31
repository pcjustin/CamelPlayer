// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <map>
#include <thread>

#include "Common/util/Text.h"
#include "FakeHttp.h"
#include "Server/Support/Updates.h"
#include "Server/Support/ZipWriter.h"
#include "TestUtil.h"
#include "Updater/Install.h"
#include "Updater/UpdateFiles.h"

using namespace camel::server;
namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using FakeGitHub = testutil::FakeHttp;

// Points the update check at `server` and the temp folder into build\testtmp for one test.
class Redirected {
public:
    Redirected(const FakeGitHub& server, const fs::path& temp) {
        const std::string base = server.base();
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

std::string releaseJson(const std::string& base, const std::string& tag, bool prerelease = false) {
    const std::string version = tag.substr(1);
    return R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"body":"Notes","assets":[{"name":"CamelPlayer-)" + version + R"(-win-x64.zip","browser_download_url":")" + base +
           R"(/zip"},{"name":"CamelPlayer-)" + version + R"(-SHA256SUMS.txt","browser_download_url":")" + base + R"(/sums"}]})";
}

std::string makeZip(const fs::path& file, const std::map<std::string, std::string>& entries) {
    {
        ZipWriter zip(file);
        for (const auto& [name, data] : entries) zip.add(name, data);
        zip.finish();
    }
    return readText(file);
}

} // namespace

TEST(Update, ParsesReleaseVersions) {
    EXPECT_EQ(update::parseVersion("v0.1.10"), (update::Version{0, 1, 10}));
    EXPECT_EQ(update::parseVersion("2.0.0"), (update::Version{2, 0, 0}));
    for (const char* bad : {"", "v", "0.1", "0.1.0-beta.1", "v0.1.0.1", "0..1", "a.b.c", "v-1.0.0"})
        EXPECT_FALSE(update::parseVersion(bad)) << bad;
    EXPECT_LT(*update::parseVersion("0.9.9"), *update::parseVersion("0.10.0"));
}

TEST(Update, OffersOnlyNewerStableReleasesWithBothAssets) {
    const std::string base = "https://github.com/x";
    const auto release = update::newerRelease(releaseJson(base, "v0.1.1"), "0.1.0");
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, "0.1.1");
    EXPECT_EQ(release->notes, "Notes");
    EXPECT_EQ(release->zipName, "CamelPlayer-0.1.1-win-x64.zip");
    EXPECT_EQ(release->zipUrl, base + "/zip");
    EXPECT_EQ(release->sumsUrl, base + "/sums");
    EXPECT_FALSE(update::newerRelease(releaseJson(base, "v0.1.0"), "0.1.0"));
    EXPECT_FALSE(update::newerRelease(releaseJson(base, "v0.0.9"), "0.1.0"));
    EXPECT_FALSE(update::newerRelease(releaseJson(base, "v0.2.0", true), "0.1.0"));
    EXPECT_FALSE(update::newerRelease(R"({"tag_name":"v0.2.0","assets":[]})", "0.1.0"));
    EXPECT_FALSE(update::newerRelease(releaseJson("http://example.com", "v0.2.0"), "0.1.0"));
    EXPECT_TRUE(update::newerRelease(releaseJson("http://127.0.0.1:1", "v0.2.0"), "0.1.0", true));
    // The 0.0.1 release on GitHub predates the versioned asset names.
    EXPECT_FALSE(update::newerRelease(
        R"({"tag_name":"v0.0.1","assets":[{"name":"CamelPlayer-v2-v0.0.1-win-x64.zip","browser_download_url":"https://x/a"}]})",
        "0.0.0"));
    EXPECT_THROW(update::newerRelease("not json", "0.1.0"), std::exception);
}

TEST(Update, ReadsChecksumsAndChangelogSections) {
    const std::string hash(64, 'a');
    const std::string sums = std::string(64, 'b') + "  CamelPlayer-0.1.1-win-x64-pdb.zip\r\n" +
                             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA *CamelPlayer-0.1.1-win-x64.zip\n";
    EXPECT_EQ(update::checksumFor(sums, "CamelPlayer-0.1.1-win-x64.zip"), hash);
    EXPECT_FALSE(update::checksumFor(sums, "other.zip"));
    EXPECT_FALSE(update::checksumFor("xyz  CamelPlayer-0.1.1-win-x64.zip\n", "CamelPlayer-0.1.1-win-x64.zip"));

    const std::string changelog = "# Changelog\r\n\r\n## 0.1.10\r\n\r\n- Ten\r\n\r\n## 0.1.1 (2026-11-01)\r\n\r\n- One\r\n- "
                                  "Two\r\n\r\n## 0.1.0\r\n- Zero\r\n";
    EXPECT_EQ(update::changelogSince(changelog, "0.1.0", "0.1.1"), "- One\n- Two");
    EXPECT_EQ(update::changelogSince(changelog, "", "0.1.0"), "- Zero");
    EXPECT_EQ(update::changelogSince(changelog, "0.1.10", "0.2.0"), "");
    EXPECT_EQ(update::changelogSince(changelog, "0.1.0", "0.1.10"), "0.1.10\n- Ten\n\n0.1.1\n- One\n- Two")
        << "skipped versions are included";
    EXPECT_EQ(update::changelogSince(changelog, "0.1.1", "0.1.10"), "- Ten");
}

TEST(Update, ExtractsZipsAndRefusesEntriesOutsideTheFolder) {
    const auto dir = testutil::tempDir() / L"update-extract";
    fs::remove_all(dir);
    fs::create_directories(dir);
    makeZip(dir / L"good.zip", {{"CamelPlayer.exe", "exe"},
                                {"en-us\\Microsoft.ui.xaml.dll.mui", "mui"},
                                {"licenses/a/b.txt", std::string(100000, 'z')}});
    update::extractZip(dir / L"good.zip", dir / L"out");
    EXPECT_EQ(readText(dir / L"out" / L"CamelPlayer.exe"), "exe");
    EXPECT_EQ(readText(dir / L"out" / L"en-us" / L"Microsoft.ui.xaml.dll.mui"), "mui");
    EXPECT_EQ(readText(dir / L"out" / L"licenses" / L"a" / L"b.txt"), std::string(100000, 'z'));
    for (const char* bad : {"../evil.txt", "a/../../evil.txt", "/evil.txt", "C:/evil.txt", R"(a\..\..\evil.txt)"}) {
        makeZip(dir / L"bad.zip", {{bad, "x"}});
        EXPECT_THROW(update::extractZip(dir / L"bad.zip", dir / L"bad"), std::runtime_error) << bad;
    }
    EXPECT_FALSE(fs::exists(dir / L"evil.txt"));
}

TEST(Update, ChecksDownloadsAndVerifiesAgainstAFakeGitHub) {
    FakeGitHub github;
    const auto dir = testutil::tempDir() / L"update-download";
    fs::remove_all(dir);
    Redirected redirected(github, dir / L"temp");
    const auto latest = "/repos/pcjustin/CamelPlayer/releases/latest";

    EXPECT_FALSE(update::check("0.1.0")) << "no release published yet (404)";
    github.route(latest, releaseJson(github.base(), "v0.1.0"));
    EXPECT_FALSE(update::check("0.1.0"));
    github.route(latest, releaseJson(github.base(), "v0.1.1"));
    const auto release = update::check("0.1.0");
    ASSERT_TRUE(release);

    const std::string zip = makeZip(dir / L"source.zip", {{"CamelPlayer.exe", "new"}, {"CamelUpdater.exe", "updater"}});
    fs::create_directories(dir / L"hash");
    writeText(dir / L"hash" / L"z", zip);
    const auto hash = update::sha256File(dir / L"hash" / L"z");
    github.route("/zip", zip);
    github.route("/sums", hash + "  " + release->zipName + "\n");
    uint64_t lastDone = 0, lastTotal = 0;
    const auto files = update::prepare(*release, [&](uint64_t done, uint64_t total) { lastDone = done, lastTotal = total; }, {});
    EXPECT_EQ(files, dir / L"temp" / L"CamelPlayer-update" / L"0.1.1" / L"files");
    EXPECT_EQ(readText(files / L"CamelPlayer.exe"), "new");
    EXPECT_EQ(lastDone, zip.size());
    EXPECT_EQ(lastTotal, zip.size());

    github.route("/sums", std::string(64, '0') + "  " + release->zipName + "\n");
    EXPECT_THROW(update::prepare(*release, {}, {}), std::runtime_error);
    EXPECT_FALSE(fs::exists(dir / L"temp" / L"CamelPlayer-update" / L"0.1.1")) << "a bad download is deleted";

    github.route("/sums", hash + "  " + release->zipName + "\n");
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_THROW(update::prepare(*release, {}, cancelled), std::runtime_error);
    github.route("/zip", "gone", 404);
    EXPECT_THROW(update::prepare(*release, {}, {}), std::runtime_error);
}

TEST(Update, CleansTheTempFolderOnlyWhenNoUpdateIsRunning) {
    FakeGitHub github;
    const auto dir = testutil::tempDir() / L"update-clean";
    fs::remove_all(dir);
    Redirected redirected(github, dir / L"temp");
    const auto data = dir / L"data";
    writeText(update::tempDirectory() / L"0.1.1" / L"x", "x");
    writeText(data / camel::updatefiles::kResult, "installed 0.1.1\n");
    ASSERT_EQ(update::readResult(data)->kind, update::Result::Kind::Installed);
    update::cleanTemp(data);
    EXPECT_TRUE(fs::exists(update::tempDirectory()));
    EXPECT_TRUE(update::signalStarted(data));
    EXPECT_FALSE(update::signalStarted(data)) << "a second start finds the first signal: the updater has gone";
    EXPECT_FALSE(update::readResult(data));
    EXPECT_FALSE(fs::exists(data / camel::updatefiles::kStarted));
    update::cleanTemp(data);
    EXPECT_FALSE(fs::exists(update::tempDirectory()));
}

namespace {

// An old installation, the new version's files and a data folder; the fake player starts by signalling when the
// program folder holds `goodPlayer`.
struct InstallFixture {
    fs::path dir, app, files, data;
    camel::updater::Options options;
    camel::updater::Host host;
    std::vector<std::string> started;
    std::string goodPlayer = "new";
    std::optional<update::Result> resultSeen; // what the new player read when it started

    explicit InstallFixture(const wchar_t* name) : dir(testutil::tempDir() / name) {
        fs::remove_all(dir);
        app = dir / L"CamelPlayer";
        files = dir / L"files";
        data = dir / L"data";
        writeText(app / L"CamelPlayer.exe", "old");
        writeText(app / L"old-only.dll", "old");
        writeText(app / L"en-us" / L"a.mui", "old");
        writeText(files / L"CamelPlayer.exe", "new");
        writeText(files / L"CamelUpdater.exe", "new");
        writeText(files / L"en-us" / L"a.mui", "new");
        writeText(data / L"settings.json", "settings-old");
        writeText(data / L"library.db", "db-old");
        options = {app, files, data, dir / L"temp" / L"data-backup", L"0.1.1", L"0.1.0"};
        host.startTimeout = std::chrono::milliseconds(1500);
        host.launchGrace = std::chrono::milliseconds(0);
        host.playerRunning = [] { return true; };
        host.endPlayer = [] {};
        host.startPlayer = [this](const fs::path& exe) {
            const auto which = readText(exe);
            started.push_back(which);
            if (which != "new") return;
            resultSeen = update::readResult(data);
            writeText(data / L"settings.json", "settings-new"); // the new version migrates its data first
            writeText(data / L"library.db-wal", "wal-new");
            if (goodPlayer == "new") writeText(data / camel::updatefiles::kStarted, "1");
        };
    }

    void expectOldVersionRestored() const {
        EXPECT_EQ(readText(app / L"CamelPlayer.exe"), "old");
        EXPECT_EQ(readText(app / L"old-only.dll"), "old");
        EXPECT_EQ(readText(app / L"en-us" / L"a.mui"), "old");
        EXPECT_FALSE(fs::exists(app / L"CamelUpdater.exe"));
        EXPECT_FALSE(fs::exists(app / camel::updatefiles::kBackup));
        EXPECT_EQ(readText(data / L"settings.json"), "settings-old");
        EXPECT_EQ(readText(data / L"library.db"), "db-old");
        EXPECT_FALSE(fs::exists(data / L"library.db-wal"));
        EXPECT_FALSE(fs::exists(options.dataBackup));
        const auto result = update::readResult(data);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->kind, update::Result::Kind::Failed);
        EXPECT_EQ(result->version, "0.1.1");
    }
};

} // namespace

TEST(Update, InstallsTheNewVersionAndRemovesTheBackups) {
    InstallFixture f(L"update-install");
    EXPECT_EQ(camel::updater::install(f.options, f.host), camel::updater::Outcome::Installed);
    EXPECT_EQ(f.started, std::vector<std::string>{"new"});
    ASSERT_TRUE(f.resultSeen);
    EXPECT_EQ(f.resultSeen->kind, update::Result::Kind::Installed);
    EXPECT_EQ(f.resultSeen->version, "0.1.1");
    EXPECT_EQ(f.resultSeen->previous, "0.1.0");
    EXPECT_EQ(readText(f.app / L"CamelPlayer.exe"), "new");
    EXPECT_EQ(readText(f.app / L"en-us" / L"a.mui"), "new");
    EXPECT_FALSE(fs::exists(f.app / L"old-only.dll")) << "files the new version no longer has are gone";
    EXPECT_FALSE(fs::exists(f.app / camel::updatefiles::kBackup));
    EXPECT_FALSE(fs::exists(f.options.dataBackup));
    EXPECT_EQ(readText(f.data / L"settings.json"), "settings-new");
    EXPECT_FALSE(update::readResult(f.data));
    EXPECT_FALSE(fs::exists(f.data / camel::updatefiles::kStarted));
}

TEST(Update, RollsBackWhenTheNewVersionDoesNotStart) {
    InstallFixture f(L"update-no-start");
    f.goodPlayer = "none";
    EXPECT_EQ(camel::updater::install(f.options, f.host), camel::updater::Outcome::RolledBack);
    EXPECT_EQ(f.started, (std::vector<std::string>{"new", "old"}));
    f.expectOldVersionRestored();
}

TEST(Update, RollsBackWhenTheNewVersionExitsEarly) {
    InstallFixture f(L"update-exits");
    f.goodPlayer = "none";
    f.host.startTimeout = std::chrono::milliseconds(60000);
    bool ended = false;
    f.host.playerRunning = [] { return false; };
    f.host.endPlayer = [&ended] { ended = true; };
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(camel::updater::install(f.options, f.host), camel::updater::Outcome::RolledBack);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
    EXPECT_FALSE(ended);
    f.expectOldVersionRestored();
}

TEST(Update, RollsBackWhenCopyingFails) {
    InstallFixture f(L"update-copy-fails");
    writeText(f.files / L"zz-locked.dll", "new");
    const HANDLE locked = CreateFileW((f.files / L"zz-locked.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_EQ(camel::updater::install(f.options, f.host), camel::updater::Outcome::RolledBack);
    CloseHandle(locked);
    EXPECT_EQ(f.started, std::vector<std::string>{"old"});
    EXPECT_FALSE(fs::exists(f.app / L"zz-locked.dll"));
    EXPECT_EQ(readText(f.data / L"settings.json"), "settings-old");
    EXPECT_EQ(readText(f.app / L"CamelPlayer.exe"), "old");
    EXPECT_EQ(update::readResult(f.data)->kind, update::Result::Kind::Failed);
}

// Runs against the real GitHub API; enable with --gtest_also_run_disabled_tests.
TEST(Update, DISABLED_ReachesTheRealGitHubApi) {
    const auto body = update::httpGet(update::latestReleaseUrl());
    EXPECT_NE(body.find("tag_name"), std::string::npos);
    EXPECT_FALSE(update::check("99.0.0"));
}

TEST(Update, ParsesInstallManifestsAndRefusesUnsafePaths) {
    const std::string hash(64, 'a');
    const auto entries = update::parseManifest(hash + "  12  CamelPlayer.exe\r\n\r\n" + hash + "  0  en-us/Microsoft UI.mui\n");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].sha256, hash);
    EXPECT_EQ(entries[0].size, 12u);
    EXPECT_EQ(entries[0].path, fs::path(L"CamelPlayer.exe"));
    EXPECT_EQ(entries[1].path, fs::path(L"en-us") / L"Microsoft UI.mui");
    for (const auto& bad : std::vector<std::string>{hash + "  12  ../evil.dll", hash + "  12  C:/evil.dll", hash + "  x  a.dll",
                                                    hash + "  12", "abc  12  a.dll", std::string(64, 'G') + "  12  a.dll"})
        EXPECT_THROW(update::parseManifest(bad), std::runtime_error) << bad;
}

TEST(Update, ChecksAnInstallationAgainstItsManifest) {
    const auto app = testutil::tempDir() / L"install-check";
    fs::remove_all(app);
    writeText(app / L"CamelPlayer.exe", "player");
    writeText(app / L"en-us" / L"a.mui", "mui");
    writeText(app / L"gone.dll", "gone");
    EXPECT_FALSE(update::checkInstallation(app, true)) << "a development build has no manifest";
    std::string manifest;
    for (const auto* name : {L"CamelPlayer.exe", L"en-us/a.mui", L"gone.dll"})
        manifest +=
            update::sha256File(app / name) + "  " + std::to_string(fs::file_size(app / name)) + "  " + camel::toUtf8(name) + "\n";
    writeText(app / camel::updatefiles::kManifest, manifest);
    writeText(app / L"extra.txt", "not listed");

    auto check = update::checkInstallation(app, true);
    ASSERT_TRUE(check);
    EXPECT_TRUE(check->intact());
    EXPECT_EQ(check->files, 3u);

    fs::remove(app / L"gone.dll");
    writeText(app / L"en-us" / L"a.mui", "MUI"); // same size, other bytes
    check = update::checkInstallation(app, false);
    ASSERT_TRUE(check);
    EXPECT_EQ(check->missing, std::vector<fs::path>{L"gone.dll"});
    EXPECT_TRUE(check->damaged.empty()) << "the quick check compares sizes only";
    check = update::checkInstallation(app, true);
    EXPECT_EQ(check->damaged, std::vector<fs::path>{fs::path(L"en-us") / L"a.mui"});
    writeText(app / L"CamelPlayer.exe", "short");
    EXPECT_EQ(update::checkInstallation(app, false)->damaged, std::vector<fs::path>{L"CamelPlayer.exe"});

    const auto text = update::describeInstallation(app);
    EXPECT_NE(text.find("3 files listed, 1 missing, 2 damaged"), std::string::npos) << text;
    EXPECT_NE(text.find("missing: gone.dll"), std::string::npos) << text;
    EXPECT_NE(text.find("damaged: en-us/a.mui"), std::string::npos) << text;

    const auto data = app / L"data";
    fs::create_directories(data);
    EXPECT_FALSE(update::verified(data, "0.1.0"));
    update::markVerified(data, "0.1.0");
    EXPECT_TRUE(update::verified(data, "0.1.0"));
    EXPECT_FALSE(update::verified(data, "0.1.1")) << "a new version is checked fully again";
}

TEST(Update, FindsTheRunningVersionsReleaseForARepair) {
    FakeGitHub github;
    const auto dir = testutil::tempDir() / L"update-repair";
    fs::remove_all(dir);
    Redirected redirected(github, dir / L"temp");
    const auto tag = "/repos/pcjustin/CamelPlayer/releases/tags/v0.1.0";

    EXPECT_FALSE(update::releaseOf("0.1.0")) << "no longer on GitHub (404)";
    github.route(tag, releaseJson(github.base(), "v0.1.0", true));
    EXPECT_FALSE(update::releaseOf("0.1.0")) << "not a pre-release";
    github.route(tag, releaseJson(github.base(), "v0.1.1"));
    EXPECT_FALSE(update::releaseOf("0.1.0")) << "only the same version";
    github.route(tag, releaseJson(github.base(), "v0.1.0"));
    const auto release = update::releaseOf("0.1.0");
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, "0.1.0");
    EXPECT_EQ(release->zipName, "CamelPlayer-0.1.0-win-x64.zip");
    EXPECT_FALSE(update::exactRelease(releaseJson("http://example.com", "v0.1.0"), "0.1.0"));
}

TEST(Update, KeepsUnlistedFilesWhenRepairing) {
    const auto dir = testutil::tempDir() / L"update-keep";
    fs::remove_all(dir);
    const auto app = dir / L"app", files = dir / L"files";
    writeText(app / L"CamelPlayer.exe", "damaged");
    writeText(app / L"plugins" / L"mine.dll", "mine");
    writeText(app / L"notes.txt", "notes");
    writeText(app / camel::updatefiles::kBackup / L"old.dll", "backup");
    writeText(files / L"CamelPlayer.exe", "release");
    update::keepUnlistedFiles(app, files);
    EXPECT_EQ(readText(files / L"CamelPlayer.exe"), "release");
    EXPECT_EQ(readText(files / L"plugins" / L"mine.dll"), "mine");
    EXPECT_EQ(readText(files / L"notes.txt"), "notes");
    EXPECT_FALSE(fs::exists(files / camel::updatefiles::kBackup));
}
