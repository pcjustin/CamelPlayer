// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the Server shows on Discord (docs/Plan.md §8.4): the playing track from each published snapshot, and its cover
// through the tunnel while covers are on.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Server/Api/ServerApi.h"
#include "Server/Integration/CoverTunnel.h"
#include "Server/Integration/DiscordRpc.h"

namespace camel::server {

// The format line of the small image's text, such as "FLAC 24-bit 96 kHz"; empty without a source.
std::string discordFormat(const PlaybackSnapshot& playback);
// The activity for a snapshot, or nullopt while nothing plays; `nowMs` is the Unix time in ms.
std::optional<DiscordActivity> discordActivity(const Snapshot& snapshot, const DiscordTexts& texts, int64_t nowMs);

class DiscordPresence {
public:
    // The playing file's cover as a 512 px square JPEG, or nothing; it may read slow storage.
    using CoverLoader = std::function<std::vector<uint8_t>(const std::string& path, const CancellationToken& token)>;

    DiscordPresence(std::filesystem::path cloudflared, CoverLoader loader, DiscordClient::Options client = {},
                    CoverTunnel::Options tunnel = {});
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    void configure(const DiscordSettings& settings);
    void setTexts(const DiscordTexts& texts);
    // Cheap enough for every published snapshot: Discord hears only of changes.
    void observe(const Snapshot& snapshot);
    DiscordStatus status() const;

private:
    void loadCovers(const std::stop_token& stop);

    CoverLoader loader_;
    CoverTunnel::Options tunnelOptions_;
    mutable std::mutex mutex_;
    std::condition_variable_any coverWanted_;
    DiscordSettings settings_;
    DiscordTexts texts_;
    std::string track_; // the queue entry and file shown, to notice a track change
    std::chrono::steady_clock::time_point trackSince_;
    std::string coverFor_;    // the file whose cover is wanted
    uint64_t coverAsked_ = 0; // counts the times a cover was wanted, the same file's again among them
    std::string servedFor_;   // the file whose cover the tunnel serves (or found none)
    bool servedCover_ = false;
    CancellationToken coverCancel_;
    std::unique_ptr<CoverTunnel> tunnel_; // made the first time covers are on
    DiscordClient client_;
    std::jthread coverWorker_;
};

} // namespace camel::server
