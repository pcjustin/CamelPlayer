// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Automatic updates from GitHub Releases (docs/Plan.md §8.6): the check, the verified download and the hand-off to
// CamelUpdater.exe, which replaces the program folder once the player has exited.
#pragma once
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "Server/Artwork/ArtworkProvider.h"

namespace camel::server::update {

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
// "1.2.3" or "v1.2.3"; nothing else is a release version.
std::optional<Version> parseVersion(std::string_view text);

struct Release {
    std::string version;
    std::string notes; // the GitHub release body
    std::string zipName, zipUrl, sumsUrl;
};

// The /releases/latest response when it is a stable release newer than `current` with the zip and the SHA-256 sums
// among its assets; asset links must be https unless `allowHttp`. Throws on malformed JSON.
std::optional<Release> newerRelease(const std::string& json, std::string_view current, bool allowHttp = false);
// The lowercase SHA-256 the sums file lists for `fileName` ("<hex>  <name>" lines).
std::optional<std::string> checksumFor(std::string_view sums, std::string_view fileName);
std::string sha256File(const std::filesystem::path& file); // throws std::runtime_error

// Extracts every entry (stored or deflated, '/' or '\' separators) below `directory`; entries that would land outside
// it are refused. Throws std::runtime_error.
void extractZip(const std::filesystem::path& zip, const std::filesystem::path& directory);

// https://api.github.com, or CAMELPLAYER_UPDATE_URL when it names a local test server (http://127.0.0.1:… or
// http://localhost:…).
std::string apiBase();
std::string latestReleaseUrl();

using Progress = std::function<void(uint64_t done, uint64_t total)>;
// Throws std::runtime_error on network errors, HTTP statuses other than 200 and cancellation.
std::string httpGet(const std::string& url, const CancellationToken& token = {});
void download(const std::string& url, const std::filesystem::path& file, const Progress& progress,
              const CancellationToken& token);

// The newer release, or nothing when the repository has none (404) or only older ones. Throws on failures.
std::optional<Release> check(std::string_view current);

// %TEMP%\CamelPlayer-update: downloads, the extracted files, the data snapshot and the running updater.
std::filesystem::path tempDirectory();
// Downloads the zip and sums into tempDirectory()\<version>, verifies the zip and extracts it; returns the folder with
// the new program files. Throws std::runtime_error.
std::filesystem::path prepare(const Release& release, const Progress& progress, const CancellationToken& token);
// Copies CamelUpdater.exe from `programDirectory` into tempDirectory() and starts it for this process, which runs
// `current`. The caller then exits the normal way. Throws std::runtime_error.
void startUpdater(const std::filesystem::path& programDirectory, const std::filesystem::path& files,
                  const std::filesystem::path& dataDirectory, std::string_view version, std::string_view current);
// Removes tempDirectory() when no update is in progress (no update-result file in `dataDirectory`).
void cleanTemp(const std::filesystem::path& dataDirectory);

// What the updater left in the data folder for the player (docs/Plan.md §8.6).
struct Result {
    enum class Kind : uint8_t { Installed, Failed, Cancelled } kind;
    std::string version;
    std::string previous; // the replaced version, for Installed
};
std::optional<Result> readResult(const std::filesystem::path& dataDirectory);
void removeResult(const std::filesystem::path& dataDirectory);
// The new player tells the updater it started: its window is shown and the Server has opened the data. False when a
// signal from an earlier start is still there, so the updater has gone and the result is stale; both are removed then.
bool signalStarted(const std::filesystem::path& dataDirectory);
// The CHANGELOG.md sections `## <v>` with `previous` < v <= `version` (only `version` when `previous` is not a
// version). A single section comes without its heading; several are each headed by their version.
std::string changelogSince(const std::string& changelog, std::string_view previous, std::string_view version);

} // namespace camel::server::update
