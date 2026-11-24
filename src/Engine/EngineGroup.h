// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A zone's Engine sessions (docs/Plan.md §5.12): its own, and while it is a group of this computer's outputs one for each
// other member too, each with the member's own format, DSP and volume. Every command goes to each; what the zone shows
// is the clock member's, whose session leads, and a member that fails leaves the others playing, a clock passing to the
// next.
#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "Engine/GroupSync.h"
#include "Engine/PlaybackEngine.h"

namespace camel::audio {

class EngineGroup {
public:
    struct Member {
        std::string key;   // the member's zone, as "asio:<id>"
        Settings settings; // its own output's
        int32_t delayMs = 0;
    };
    struct MemberState {
        std::string key;
        bool clock = false;
        std::optional<ErrorCode> lost; // why it stopped while the others play on
        std::string detail;
        bool operator==(const MemberState&) const = default;
    };

    EngineGroup(const Settings& settings, OutputFactory factory, DeviceWatcherFactory watcher = watchDefaultRenderDevice);
    ~EngineGroup();
    EngineGroup(const EngineGroup&) = delete;
    EngineGroup& operator=(const EngineGroup&) = delete;

    // The zone's own output first, then the other members; `clock` names the one the others keep to. One member plays
    // alone. A member that joins starts where the clock is.
    void setMembers(std::vector<Member> members, size_t clock);
    std::vector<MemberState> members() const;
    // The followers keep to the clock's file and state, and a clock that failed passes to the next member. Called often.
    void tend();

    void openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt, double gainDb = 0.0,
                     bool afterEnd = false);
    void crossfadeTo(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt, double gainDb = 0.0);
    void load(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt, double gainDb = 0.0);
    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(uint64_t frame);
    // The zone's own output's settings.
    void applySettings(const Settings& settings);
    bool showControlPanel();
    bool queueNext(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt, double gainDb = 0.0,
                   uint32_t crossfadeMs = 0);
    bool clearNext();
    void discardPreloads();

    EngineSnapshot snapshot() const;
    void setListener(std::function<void()> listener);
    void waitIdle();
    void waitIdle(std::chrono::milliseconds period, const std::function<void()>& meanwhile);
    bool waitForState(PlayState s, std::chrono::milliseconds timeout);

private:
    struct Session {
        std::string key;
        Settings settings; // as given, before a follower's own DSD becomes PCM
        int32_t delayMs = 0;
        std::shared_ptr<GroupLink> link = std::make_shared<GroupLink>();
        std::unique_ptr<PlaybackEngine> engine;
        std::optional<ErrorCode> lost;
        std::string detail;
        std::chrono::steady_clock::time_point resynced;
    };

    std::unique_ptr<PlaybackEngine> makeEngine(const std::shared_ptr<GroupLink>& link, const Settings& settings) const;
    Settings sessionSettings(const Session& s, bool follower) const;
    void place(); // every session's link: alone, the clock or a follower with its delay
    void resync(Session& s, const EngineSnapshot& leader);
    template <class F> void forEach(F&& f);
    PlaybackEngine& leader() const { return *sessions_[leader_].engine; }
    double gainOf(const std::wstring& file) const;
    void remember(const std::filesystem::path& file, double gainDb);

    OutputFactory factory_;
    DeviceWatcherFactory watcher_;
    GroupClock clock_;
    mutable std::shared_mutex mutex_; // sessions_ and leader_: shared for commands, exclusive to change them
    std::vector<Session> sessions_;   // the zone's own first
    size_t leader_ = 0;
    std::function<void()> listener_;
    std::deque<std::pair<std::wstring, double>> gains_; // recent files' ReplayGain, for a follower starting afresh
};

} // namespace camel::audio
