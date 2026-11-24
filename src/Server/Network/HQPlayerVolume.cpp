// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/HQPlayerVolume.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include "Common/util/Log.h"

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kTick = std::chrono::milliseconds(100);
constexpr double kSameDb = 0.05;

double attribute(const upnp::XmlNode& node, const char* name) {
    const auto found = node.attributes.find(name);
    double value = 0.0;
    if (found != node.attributes.end()) std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    return value;
}

} // namespace

struct HQPlayerVolume::Impl {
    HQPlayerVolumeOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool enabled = false;
    double minDb = 0.0, maxDb = 0.0;
    std::optional<double> db; // as HQPlayer last reported it, or as last set
    bool muted = false;
    double keptDb = 0.0; // the level while muted
    std::optional<double> toWrite;
    Clock::time_point settledAt;
    std::atomic<bool> connected = false;
    hqplayer::Client client;
    std::thread working;

    Impl(std::string address, HQPlayerVolumeOptions volumeOptions)
        : options(volumeOptions), client(
                                      std::move(address), [this](const hqplayer::Status& s) { follow(s); },
                                      [this] { connected = false; }, options.client) {
        working = std::thread([this] { work(); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        working.join();
        client.disconnect();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void follow(const hqplayer::Status& s) {
        std::scoped_lock lock(mutex);
        if (Clock::now() < settledAt || toWrite) return;
        // Muted shows as the minimum; anything else was set in HQPlayer's window.
        if (muted && std::abs(s.volume - minDb) < kSameDb) return;
        muted = false;
        db = s.volume;
    }

    void readRange() {
        const auto range = client.send("<VolumeRange/>");
        std::scoped_lock lock(mutex);
        enabled = attribute(range, "enabled") != 0.0;
        minDb = attribute(range, "min");
        maxDb = attribute(range, "max");
        if (!db)
            if (const auto last = client.status()) db = last->volume;
    }

    // A Status that comes while the command is under way was sent before HQPlayer took it: it shows the level before, not
    // one set in HQPlayer's window, so none counts until the reply and the settling after it (OP-R64).
    void write() {
        std::optional<double> value;
        {
            std::scoped_lock lock(mutex);
            value = std::exchange(toWrite, std::nullopt);
            if (value) settledAt = Clock::time_point::max();
        }
        if (!value) return;
        bool taken = false;
        try {
            taken = client.ok(std::format(R"(<Volume value="{:.1f}"/>)", *value));
        } catch (const std::exception&) {
            std::scoped_lock lock(mutex);
            settledAt = Clock::now(); // the next connection's Status counts
            throw;
        }
        if (!taken) log::warn("HQPlayer refused the volume %.1f dB", *value);
        std::scoped_lock lock(mutex);
        settledAt = Clock::now() + options.settling;
    }

    void work() {
        auto nextTry = Clock::now();
        auto nextRange = Clock::now();
        for (;;) {
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, kTick, [&] { return stopping || toWrite; });
                if (stopping) return;
            }
            try {
                if (!connected) {
                    if (Clock::now() < nextTry) continue;
                    nextTry = Clock::now() + options.retryAfter;
                    client.connect();
                    connected = true;
                    nextRange = Clock::now();
                }
                write();
                if (Clock::now() >= nextRange) {
                    nextRange = Clock::now() + options.rangeEvery;
                    readRange();
                }
            } catch (const std::exception& e) {
                log::info("HQPlayer volume: %s", e.what());
                connected = false;
            }
        }
    }

    // The level `level` stands for, and back, in HQPlayer's range.
    double dbOf(uint32_t level) const { return std::round((minDb + (maxDb - minDb) * level / 100.0) * 10.0) / 10.0; }
    uint32_t levelOf(double value) const {
        if (maxDb <= minDb) return 100;
        return uint32_t(std::clamp(std::lround((value - minDb) * 100.0 / (maxDb - minDb)), 0L, 100L));
    }
};

HQPlayerVolume::HQPlayerVolume(std::string address, HQPlayerVolumeOptions options)
    : impl_(std::make_unique<Impl>(std::move(address), options)) {}

HQPlayerVolume::~HQPlayerVolume() = default;

std::optional<PlayerLevel> HQPlayerVolume::current() const {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->connected || !impl_->enabled || !impl_->db) return std::nullopt;
    const double shown = impl_->muted ? impl_->keptDb : *impl_->db;
    return PlayerLevel{impl_->levelOf(shown), impl_->muted, shown};
}

void HQPlayerVolume::setLevel(uint32_t level) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->connected || !impl_->enabled || !impl_->db) return;
        const double value = impl_->dbOf(std::min(level, 100U));
        if (!impl_->muted && std::abs(value - *impl_->db) < kSameDb) return;
        impl_->muted = false;
        impl_->db = impl_->toWrite = value;
    }
    impl_->wake.notify_all();
}

void HQPlayerVolume::setMuted(bool muted) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->connected || !impl_->enabled || !impl_->db || impl_->muted == muted) return;
        if (muted) {
            impl_->keptDb = *impl_->db;
            impl_->toWrite = impl_->minDb;
        } else {
            impl_->db = impl_->toWrite = impl_->keptDb;
        }
        impl_->muted = muted;
    }
    impl_->wake.notify_all();
}

} // namespace camel::server
