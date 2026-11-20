// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerVolume.h"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr auto kTick = milliseconds(25);
constexpr auto kSubscribeRetry = std::chrono::seconds(30);
constexpr auto kSettling = milliseconds(1500); // what a player reports this long after a write can be older than the write

bool mutedFrom(const std::string& text) {
    return text == "1" || _stricmp(text.c_str(), "true") == 0;
}

} // namespace

struct PlayerVolume::Impl {
    upnp::PlayerDescription player;
    LinkOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::optional<PlayerLevel> level;
    std::optional<uint32_t> levelToWrite;
    std::optional<bool> muteToWrite;
    Clock::time_point settledAt;
    std::unique_ptr<PlayerEvents> events;
    // The player's own scale of its volume, as its description declares it; read on the worker thread.
    std::pair<int64_t, int64_t> range{0, 100};
    bool rangeRead = false;
    std::thread working;

    Impl(upnp::PlayerDescription description, LinkOptions linkOptions) : player(std::move(description)), options(linkOptions) {
        working = std::thread([this] { work(); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        working.join();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool settled() const { return Clock::now() >= settledAt && !levelToWrite && !muteToWrite; }

    upnp::SoapReply call(const char* action, const std::vector<std::pair<std::string, std::string>>& extra) const {
        const auto* service = player.service("RenderingControl");
        if (!service) throw std::runtime_error("the player has no RenderingControl");
        std::vector<std::pair<std::string, std::string>> arguments{{"InstanceID", "0"}, {"Channel", "Master"}};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return upnp::call(*service, action, arguments, options.requestTimeoutMs);
    }

    // A description without a range for Volume leaves 0 to 100; one that cannot be fetched is asked for again.
    void readRange() {
        if (rangeRead) return;
        const auto* service = player.service("RenderingControl");
        if (service && !service->scpdUrl.empty()) {
            const auto reply = http::send("GET", service->scpdUrl, "", "", options.requestTimeoutMs, upnp::kMaxMessageBytes);
            if (reply.status == 200)
                if (const auto declared = upnp::parseVariableRange(reply.body, "Volume")) range = *declared;
        }
        rangeRead = true;
    }

    uint32_t levelFrom(const std::string& text) const {
        int64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        const auto [minimum, maximum] = range;
        return uint32_t(((std::clamp(value, minimum, maximum) - minimum) * 100 + (maximum - minimum) / 2) / (maximum - minimum));
    }

    std::string valueOf(uint32_t level) const {
        const auto [minimum, maximum] = range;
        return std::to_string(minimum + (int64_t(level) * (maximum - minimum) + 50) / 100);
    }

    bool read() {
        readRange();
        const auto volume = call("GetVolume", {});
        const auto mute = call("GetMute", {});
        const auto current = volume.values.find("CurrentVolume");
        if (!volume.ok || current == volume.values.end()) return false;
        PlayerLevel seen{levelFrom(current->second), false};
        if (const auto muted = mute.values.find("CurrentMute"); mute.ok && muted != mute.values.end())
            seen.muted = mutedFrom(muted->second);
        std::scoped_lock lock(mutex);
        if (settled()) level = seen;
        return true;
    }

    void write() {
        std::optional<uint32_t> newLevel;
        std::optional<bool> newMute;
        {
            std::scoped_lock lock(mutex);
            newLevel = std::exchange(levelToWrite, std::nullopt);
            newMute = std::exchange(muteToWrite, std::nullopt);
        }
        if (!newLevel && !newMute) return;
        try {
            if (newLevel) call("SetVolume", {{"DesiredVolume", valueOf(*newLevel)}});
            if (newMute) call("SetMute", {{"DesiredMute", *newMute ? "1" : "0"}});
        } catch (const std::exception& e) {
            log::warn("network player volume: %s", e.what());
        }
        std::scoped_lock lock(mutex);
        settledAt = Clock::now() + kSettling;
    }

    void follow(const PlayerEvents::Notification& notification) {
        if (notification.kind != "RenderingControl") return;
        const auto values = upnp::parsePropertySet(notification.body);
        const auto change = values.find("LastChange");
        if (change == values.end()) return;
        const auto variables = upnp::parseLastChange(change->second);
        std::scoped_lock lock(mutex);
        if (!level || !settled()) return;
        if (const auto volume = variables.find("Volume"); volume != variables.end()) level->level = levelFrom(volume->second);
        if (const auto mute = variables.find("Mute"); mute != variables.end()) level->muted = mutedFrom(mute->second);
    }

    void followEvents(Clock::time_point& nextSubscribe) {
        try {
            if (!events && Clock::now() >= nextSubscribe) {
                nextSubscribe = Clock::now() + kSubscribeRetry;
                events = std::make_unique<PlayerEvents>(player, std::vector<std::string>{"RenderingControl"}, options);
            }
            if (!events) return;
            for (const auto& notification : events->take()) follow(notification);
            events->renewDue();
        } catch (const std::exception& e) {
            log::info("network player volume: %s", e.what());
            events.reset();
        }
    }

    void work() {
        int failures = 0;
        auto nextRead = Clock::now();
        auto nextSubscribe = Clock::now();
        for (;;) {
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, kTick, [&] { return stopping || levelToWrite || muteToWrite; });
                if (stopping) break;
            }
            write();
            followEvents(nextSubscribe);
            if (Clock::now() < nextRead) continue;
            nextRead = Clock::now() + options.pollInterval;
            try {
                failures = read() ? 0 : failures + 1;
            } catch (const std::exception& e) {
                log::info("network player volume: %s", e.what());
                ++failures;
            }
            if (failures >= options.failuresUntilLost) {
                std::scoped_lock lock(mutex);
                level.reset();
            }
        }
        events.reset();
    }
};

PlayerVolume::PlayerVolume(upnp::PlayerDescription player, LinkOptions options)
    : impl_(std::make_unique<Impl>(std::move(player), options)) {}

PlayerVolume::~PlayerVolume() = default;

std::optional<PlayerLevel> PlayerVolume::current() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->level;
}

void PlayerVolume::setLevel(uint32_t level) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->level || impl_->level->level == level) return;
        impl_->level->level = level;
        impl_->levelToWrite = level;
    }
    impl_->wake.notify_all();
}

void PlayerVolume::setMuted(bool muted) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->level || impl_->level->muted == muted) return;
        impl_->level->muted = muted;
        impl_->muteToWrite = muted;
    }
    impl_->wake.notify_all();
}

} // namespace camel::server
