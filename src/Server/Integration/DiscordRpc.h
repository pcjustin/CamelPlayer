// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Discord Rich Presence over the Discord desktop app's local IPC (docs/Plan.md §8.4).
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace camel::server {

// What the activity shows while a track plays.
struct DiscordActivity {
    std::string title, artists, album;
    std::string smallImage, smallText; // the signal path state's asset and its text
    std::string largeImage;            // a cover URL; empty shows the CamelPlayer asset
    int64_t startMs = 0;               // Unix time in ms the track would have started, playing on from where it is
    int64_t endMs = 0;                 // 0 when the length is unknown
    bool operator==(const DiscordActivity&) const = default;
};

// A text Discord accepts: one character gets a space, more than 128 UTF-16 units are cut, empty stays empty.
std::string discordText(const std::string& text);
// SET_ACTIVITY's activity as JSON; without `statusDisplay` it leaves out the field older Discord versions refuse.
std::string discordActivityJson(const DiscordActivity& activity, bool statusDisplay = true);
// Whether showing `next` instead of `shown` needs an update: anything but the timestamps differs, or they moved by more
// than a seek's 5 s, which position updates never do.
bool discordActivityChanged(const std::optional<DiscordActivity>& shown, const std::optional<DiscordActivity>& next);

struct DiscordClientOptions {
    std::wstring pipePrefix = L"\\\\.\\pipe\\discord-ipc-";
    std::string clientId = "1566326816900251688"; // the "CamelPlayer" Discord application
    std::chrono::milliseconds reconnect{15000}, retry{5000}, merge{300}, timeout{5000};
};

// Keeps the activity Discord shows in step with show(): connects to the first of discord-ipc-0 to -9 that answers,
// reconnects every `reconnect` while none does, merges calls within `merge`, and retries a refused update after `retry`.
class DiscordClient {
public:
    using Options = DiscordClientOptions;
    enum class State : uint8_t { Off, Searching, Connected };

    explicit DiscordClient(Options options = {});
    ~DiscordClient();
    DiscordClient(const DiscordClient&) = delete;
    DiscordClient& operator=(const DiscordClient&) = delete;

    // Off clears the activity and closes the pipe.
    void setEnabled(bool enabled);
    // nullopt clears the activity.
    void show(const std::optional<DiscordActivity>& activity);
    State state() const;

private:
    void run(const std::stop_token& stop);
    bool connect();
    bool send(const std::optional<DiscordActivity>& activity);
    bool drain();
    void close();

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool enabled_ = false;
    bool dirty_ = false;   // `wanted_` has not reached Discord yet
    uint64_t version_ = 0; // counts show() and setEnabled() calls, which wake the worker
    std::optional<DiscordActivity> wanted_;
    std::chrono::steady_clock::time_point sendAt_, connectAt_;
    State state_ = State::Off;
    // Only the worker touches these.
    void* pipe_ = nullptr;
    uint64_t nonce_ = 0;
    bool statusDisplay_ = true; // dropped for the run once Discord refuses it
    std::jthread worker_;
};

} // namespace camel::server
