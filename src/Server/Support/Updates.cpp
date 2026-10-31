// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/Updates.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "Common/util/Text.h"
#include "Common/util/Zip.h"
#include "Server/Support/Http.h"
#include "Updater/UpdateFiles.h"

namespace camel::server::update {

namespace {

namespace files = camel::updatefiles;

constexpr char kGitHubApi[] = "https://api.github.com";
constexpr char kLatestPath[] = "/repos/pcjustin/CamelPlayer/releases/latest";
constexpr char kTagPath[] = "/repos/pcjustin/CamelPlayer/releases/tags/v";

class NotFound : public std::runtime_error {
public:
    NotFound() : std::runtime_error("HTTP 404") {}
};

std::string versionText(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

void fail(const char* what) {
    throw std::runtime_error(std::string(what) + " failed (" + std::to_string(GetLastError()) + ")");
}

// Streams the body of a successful GET to `sink`; `onLength` receives Content-Length (0 when the server sends none).
void get(const std::string& url, const char* accept, const CancellationToken& token,
         const std::function<void(uint64_t)>& onLength, const std::function<void(const char*, size_t)>& sink) {
    http::get(
        url, accept, token,
        [&](unsigned status, uint64_t length) {
            if (status == 404) throw NotFound();
            if (status != 200) throw std::runtime_error("HTTP " + std::to_string(status));
            if (onLength) onLength(length);
        },
        sink);
}

// The entry name as a relative path below the target folder; refuses absolute paths, drives and "..".
std::filesystem::path entryPath(const std::string& name, bool utf8) {
    std::wstring wide(name.size(), L'\0');
    wide.resize(
        size_t(MultiByteToWideChar(utf8 ? CP_UTF8 : 437, 0, name.data(), int(name.size()), wide.data(), int(wide.size()))));
    for (auto& c : wide)
        if (c == L'\\') c = L'/';
    if (wide.empty() || wide.front() == L'/' || wide.find(L':') != std::wstring::npos)
        throw std::runtime_error("zip: unsafe entry name " + name);
    std::filesystem::path path;
    size_t start = 0;
    while (start <= wide.size()) {
        const size_t end = std::min(wide.find(L'/', start), wide.size());
        const std::wstring part = wide.substr(start, end - start);
        if (part == L"..") throw std::runtime_error("zip: unsafe entry name " + name);
        if (!part.empty() && part != L".") path /= part;
        start = end + 1;
    }
    return path;
}

} // namespace

std::optional<Version> parseVersion(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    Version v;
    int* fields[] = {&v.major, &v.minor, &v.patch};
    const char* at = text.data();
    const char* end = text.data() + text.size();
    for (size_t i = 0; i < std::size(fields); ++i) {
        if (i > 0) {
            if (at == end || *at != '.') return std::nullopt;
            ++at;
        }
        if (at == end || *at < '0' || *at > '9') return std::nullopt;
        const auto [next, error] = std::from_chars(at, end, *fields[i]);
        if (error != std::errc()) return std::nullopt;
        at = next;
    }
    if (at != end) return std::nullopt;
    return v;
}

namespace {

// The stable release `json` describes when `wanted` accepts its version and it has the zip and the sums.
std::optional<Release> releaseFrom(const std::string& json, bool allowHttp, const std::function<bool(const Version&)>& wanted) {
    const auto j = nlohmann::json::parse(json);
    if (j.value("draft", false) || j.value("prerelease", false)) return std::nullopt;
    const auto tagged = parseVersion(j.value("tag_name", std::string()));
    if (!tagged || !wanted(*tagged)) return std::nullopt;
    Release release;
    release.version = versionText(*tagged);
    release.notes = j.value("body", std::string());
    release.zipName = "CamelPlayer-" + release.version + "-win-x64.zip";
    const std::string sumsName = "CamelPlayer-" + release.version + "-SHA256SUMS.txt";
    const auto usable = [allowHttp](const std::string& url) {
        return url.starts_with("https://") || (allowHttp && url.starts_with("http://"));
    };
    if (const auto assets = j.find("assets"); assets != j.end() && assets->is_array())
        for (const auto& asset : *assets) {
            const auto name = asset.value("name", std::string());
            const auto url = asset.value("browser_download_url", std::string());
            if (!usable(url)) continue;
            if (name == release.zipName) release.zipUrl = url;
            if (name == sumsName) release.sumsUrl = url;
        }
    if (release.zipUrl.empty() || release.sumsUrl.empty()) return std::nullopt;
    return release;
}

} // namespace

std::optional<Release> newerRelease(const std::string& json, std::string_view current, bool allowHttp) {
    const auto installed = parseVersion(current);
    return releaseFrom(json, allowHttp, [&](const Version& v) { return installed && *installed < v; });
}

std::optional<Release> exactRelease(const std::string& json, std::string_view version, bool allowHttp) {
    const auto wanted = parseVersion(version);
    return releaseFrom(json, allowHttp, [&](const Version& v) { return wanted && *wanted == v; });
}

std::optional<std::string> checksumFor(std::string_view sums, std::string_view fileName) {
    std::istringstream lines{std::string(sums)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string hash, name;
        fields >> hash >> name;
        if (!name.empty() && name.front() == '*') name.erase(0, 1); // binary-mode marker of sha256sum
        if (name != fileName || hash.size() != 64) continue;
        for (auto& c : hash) c = char(tolower(uint8_t(c)));
        if (hash.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        return hash;
    }
    return std::nullopt;
}

std::string sha256File(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + toUtf8(file.filename().wstring()));
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)))
        throw std::runtime_error("SHA-256 unavailable");
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        if (in.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0);
    }
    std::array<uint8_t, 32> digest{};
    BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0);
    BCryptDestroyHash(hash);
    std::string hex;
    for (const auto byte : digest) {
        constexpr char kDigits[] = "0123456789abcdef";
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 15];
    }
    return hex;
}

void extractZip(const std::filesystem::path& zip, const std::filesystem::path& directory) {
    std::ifstream in(zip, std::ios::binary);
    if (!in) throw std::runtime_error("zip: cannot open the file");
    const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const auto& entry : readZip(data)) {
        const auto target = directory / entryPath(entry.name, entry.utf8);
        if (entry.directory) {
            std::filesystem::create_directories(target);
            continue;
        }
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(entry.data.data(), std::streamsize(entry.data.size()));
        file.close();
        if (!file) throw std::runtime_error("zip: cannot write " + entry.name);
    }
}

std::string apiBase() {
    wchar_t value[256] = {};
    const DWORD length = GetEnvironmentVariableW(L"CAMELPLAYER_UPDATE_URL", value, DWORD(std::size(value)));
    if (length > 0 && length < std::size(value)) {
        std::string base = toUtf8(value);
        while (base.ends_with('/')) base.pop_back();
        if (base.starts_with("http://127.0.0.1:") || base.starts_with("http://localhost:")) return base;
    }
    return kGitHubApi;
}

std::string latestReleaseUrl() {
    return apiBase() + kLatestPath;
}

std::string httpGet(const std::string& url, const CancellationToken& token) {
    std::string body;
    get(url, "application/vnd.github+json", token, {}, [&body](const char* data, size_t size) { body.append(data, size); });
    return body;
}

void download(const std::string& url, const std::filesystem::path& file, const Progress& progress,
              const CancellationToken& token) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + toUtf8(file.filename().wstring()));
    uint64_t total = 0, done = 0;
    get(
        url, "application/octet-stream", token, [&total](uint64_t length) { total = length; },
        [&](const char* data, size_t size) {
            out.write(data, std::streamsize(size));
            done += size;
            if (progress) progress(done, total);
        });
    out.close();
    if (!out) throw std::runtime_error("cannot write " + toUtf8(file.filename().wstring()));
}

std::optional<Release> check(std::string_view current) {
    std::string body;
    try {
        body = httpGet(latestReleaseUrl());
    } catch (const NotFound&) {
        return std::nullopt; // no release published yet
    }
    return newerRelease(body, current, apiBase() != kGitHubApi);
}

std::optional<Release> releaseOf(std::string_view version) {
    std::string body;
    try {
        body = httpGet(apiBase() + kTagPath + std::string(version));
    } catch (const NotFound&) {
        return std::nullopt;
    }
    return exactRelease(body, version, apiBase() != kGitHubApi);
}

std::filesystem::path tempDirectory() {
    wchar_t temp[MAX_PATH + 1] = {};
    if (!GetTempPathW(DWORD(std::size(temp)), temp)) throw std::runtime_error("no temp folder");
    return std::filesystem::path(temp) / L"CamelPlayer-update";
}

std::filesystem::path prepare(const Release& release, const Progress& progress, const CancellationToken& token) {
    const auto folder = tempDirectory() / toWide(release.version);
    std::filesystem::create_directories(folder);
    const auto zip = folder / toWide(release.zipName);
    const auto sums = folder / L"SHA256SUMS.txt";
    download(release.sumsUrl, sums, {}, token);
    download(release.zipUrl, zip, progress, token);
    std::ifstream sumsFile(sums, std::ios::binary);
    const std::string sumsText{std::istreambuf_iterator<char>(sumsFile), std::istreambuf_iterator<char>()};
    sumsFile.close();
    const auto expected = checksumFor(sumsText, release.zipName);
    std::error_code error;
    if (!expected || sha256File(zip) != *expected) {
        std::filesystem::remove_all(folder, error);
        throw std::runtime_error(expected ? "the download does not match its SHA-256 checksum"
                                          : "the checksum file does not list " + release.zipName);
    }
    const auto files = folder / L"files";
    std::filesystem::remove_all(files, error);
    extractZip(zip, files);
    if (!std::filesystem::exists(files / files::kPlayer, error)) throw std::runtime_error("the zip has no CamelPlayer.exe");
    return files;
}

void startUpdater(const std::filesystem::path& programDirectory, const std::filesystem::path& files,
                  const std::filesystem::path& dataDirectory, std::string_view version, std::string_view current) {
    const auto updater = tempDirectory() / files::kUpdater;
    std::filesystem::copy_file(programDirectory / files::kUpdater, updater, std::filesystem::copy_options::overwrite_existing);
    const auto quoted = [](const std::filesystem::path& path) {
        std::wstring text = path.wstring();
        while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
        return L"\"" + text + L"\"";
    };
    std::wstring command = quoted(updater) + L" --pid " + std::to_wstring(GetCurrentProcessId()) + L" --app " +
                           quoted(programDirectory) + L" --files " + quoted(files) + L" --data " + quoted(dataDirectory) +
                           L" --version " + toWide(std::string(version)) + L" --from " + toWide(std::string(current));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(updater.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, updater.parent_path().c_str(),
                        &startup, &process))
        fail("starting the updater");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void cleanTemp(const std::filesystem::path& dataDirectory) {
    std::error_code error;
    if (std::filesystem::exists(dataDirectory / files::kResult, error)) return;
    std::filesystem::remove_all(tempDirectory(), error);
}

std::optional<Result> readResult(const std::filesystem::path& dataDirectory) {
    std::ifstream in(dataDirectory / files::kResult);
    std::string kind, version, previous;
    if (!(in >> kind >> version)) return std::nullopt;
    in >> previous;
    if (kind == "installed") return Result{Result::Kind::Installed, version, previous};
    if (kind == "failed") return Result{Result::Kind::Failed, version, {}};
    if (kind == "cancelled") return Result{Result::Kind::Cancelled, version, {}};
    return std::nullopt;
}

void removeResult(const std::filesystem::path& dataDirectory) {
    std::error_code error;
    std::filesystem::remove(dataDirectory / files::kResult, error);
}

bool signalStarted(const std::filesystem::path& dataDirectory) {
    std::error_code error;
    if (std::filesystem::exists(dataDirectory / files::kStarted, error)) {
        std::filesystem::remove(dataDirectory / files::kStarted, error);
        removeResult(dataDirectory);
        return false;
    }
    std::ofstream(dataDirectory / files::kStarted) << 1;
    return true;
}

std::vector<ManifestEntry> parseManifest(std::string_view text) {
    std::vector<ManifestEntry> entries;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.ends_with('\r')) line.pop_back();
        if (line.empty()) continue;
        const auto hashEnd = line.find("  ");
        const auto sizeEnd = hashEnd == std::string::npos ? hashEnd : line.find("  ", hashEnd + 2);
        if (sizeEnd == std::string::npos || hashEnd != 64) throw std::runtime_error("manifest: malformed line " + line);
        ManifestEntry entry;
        entry.sha256 = line.substr(0, 64);
        if (entry.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("manifest: malformed line " + line);
        const char* first = line.data() + hashEnd + 2;
        const char* last = line.data() + sizeEnd;
        if (const auto [end, error] = std::from_chars(first, last, entry.size); error != std::errc() || end != last)
            throw std::runtime_error("manifest: malformed line " + line);
        try {
            entry.path = entryPath(line.substr(sizeEnd + 2), true);
        } catch (const std::runtime_error&) {
            throw std::runtime_error("manifest: unsafe path " + line);
        }
        if (entry.path.empty()) throw std::runtime_error("manifest: malformed line " + line);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::optional<InstallCheck> checkInstallation(const std::filesystem::path& programDirectory, bool full) {
    std::error_code error;
    const auto manifest = programDirectory / files::kManifest;
    if (!std::filesystem::exists(manifest, error)) return std::nullopt;
    std::ifstream in(manifest, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + toUtf8(manifest.filename().wstring()));
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    InstallCheck check;
    for (const auto& entry : parseManifest(text)) {
        ++check.files;
        const auto file = programDirectory / entry.path;
        const auto size = std::filesystem::file_size(file, error);
        if (error) {
            check.missing.push_back(entry.path);
            continue;
        }
        bool same = size == entry.size;
        if (same && full) {
            try {
                same = sha256File(file) == entry.sha256;
            } catch (const std::runtime_error&) {
                same = false;
            }
        }
        if (!same) check.damaged.push_back(entry.path);
    }
    return check;
}

bool verified(const std::filesystem::path& dataDirectory, std::string_view version) {
    std::ifstream in(dataDirectory / files::kVerified);
    std::string stored;
    return in >> stored && stored == version;
}

void markVerified(const std::filesystem::path& dataDirectory, std::string_view version) {
    std::ofstream(dataDirectory / files::kVerified) << version << '\n';
}

std::string describeInstallation(const std::filesystem::path& programDirectory) {
    std::optional<InstallCheck> check;
    try {
        check = checkInstallation(programDirectory, true);
    } catch (const std::exception& e) {
        return std::string("The installation could not be checked: ") + e.what() + "\n";
    }
    if (!check) return "No install-manifest.txt: a development build, or the list was removed.\n";
    std::string text = std::to_string(check->files) + " files listed, " + std::to_string(check->missing.size()) + " missing, " +
                       std::to_string(check->damaged.size()) + " damaged.\n";
    for (const auto& path : check->missing) text += "missing: " + toUtf8(path.generic_wstring()) + "\n";
    for (const auto& path : check->damaged) text += "damaged: " + toUtf8(path.generic_wstring()) + "\n";
    return text;
}

void keepUnlistedFiles(const std::filesystem::path& programDirectory, const std::filesystem::path& files) {
    namespace fs = std::filesystem;
    for (auto it = fs::recursive_directory_iterator(programDirectory); it != fs::recursive_directory_iterator(); ++it) {
        const auto relative = it->path().lexically_relative(programDirectory);
        if (it->is_directory()) {
            if (relative == files::kBackup) it.disable_recursion_pending();
            continue;
        }
        const auto target = files / relative;
        if (fs::exists(target)) continue;
        fs::create_directories(target.parent_path());
        fs::copy_file(it->path(), target);
    }
}

std::string changelogSince(const std::string& changelog, std::string_view previous, std::string_view version) {
    const auto to = parseVersion(version);
    if (!to) return {};
    const auto from = parseVersion(previous);
    const auto wanted = [&](const Version& v) { return from ? *from < v && v <= *to : v == *to; };
    const auto trimmed = [](const std::string& text) {
        const auto first = text.find_first_not_of('\n');
        const auto last = text.find_last_not_of('\n');
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    std::vector<std::pair<std::string, std::string>> sections;
    std::istringstream lines(changelog);
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        if (line.ends_with('\r')) line.pop_back();
        if (line.starts_with("## ")) {
            const std::string heading = line.substr(3, line.find(' ', 3) - 3);
            const auto v = parseVersion(heading);
            inside = v && wanted(*v);
            if (inside) sections.emplace_back(heading, std::string());
            continue;
        }
        if (inside) sections.back().second += line + "\n";
    }
    if (sections.size() == 1) return trimmed(sections.front().second);
    std::string text;
    for (const auto& [heading, body] : sections) text += (text.empty() ? "" : "\n\n") + heading + "\n" + trimmed(body);
    return text;
}

} // namespace camel::server::update
