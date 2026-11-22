// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/SharedLoad.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>

#include "Common/util/Log.h"

namespace camel::audio::load {
namespace {

// After one engine gives way the others take a moment to catch up, so their shortfalls meanwhile ask no more of it.
constexpr auto kSettle = std::chrono::seconds(2);

struct Share {
    std::optional<double> rate;
    std::map<const void*, double> reads;
};

struct Player {
    std::chrono::steady_clock::time_point started;
    bool heavy = false;
    std::function<void()> giveWay;
};

struct State {
    std::mutex mutex;
    std::map<std::wstring, Share> shares;
    std::map<const void*, Player> players;
    std::optional<std::chrono::steady_clock::time_point> gaveWay;
};

State& state() {
    static State s;
    return s;
}

} // namespace

void setShareRead(const void* reader, const std::wstring& share, double bytesPerSecond) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const bool reads = !share.empty() && bytesPerSecond > 0;
    for (auto it = s.shares.begin(); it != s.shares.end();) {
        it->second.reads.erase(reader);
        it = it->second.reads.empty() && !(reads && it->first == share) ? s.shares.erase(it) : std::next(it);
    }
    if (reads) s.shares[share].reads[reader] = bytesPerSecond;
}

double othersRead(const void* reader, const std::wstring& share) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto found = s.shares.find(share);
    if (found == s.shares.end()) return 0.0;
    double read = 0.0;
    for (const auto& [other, rate] : found->second.reads)
        if (other != reader) read += rate;
    return read;
}

std::optional<double> shareRate(const std::wstring& share) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto found = s.shares.find(share);
    return found == s.shares.end() ? std::nullopt : found->second.rate;
}

void setShareRate(const std::wstring& share, double bytesPerSecond) {
    if (share.empty()) return;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.shares[share].rate = bytesPerSecond;
}

void setPlaying(const void* engine, bool heavy, const std::function<void()>& giveWay) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto [player, added] = s.players.try_emplace(engine);
    if (added) player->second.started = std::chrono::steady_clock::now();
    player->second.heavy = heavy;
    player->second.giveWay = giveWay;
}

void setNotPlaying(const void* engine) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.players.erase(engine);
}

Shortfall ranShort(const void* engine) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const bool others = std::ranges::any_of(s.players, [&](const auto& player) { return player.first != engine; });
    const Player* last = nullptr;
    for (const auto& [id, player] : s.players)
        if (player.heavy && (!last || player.started > last->started)) last = &player;
    if (!others || !last) return Shortfall::Alone;
    const auto now = std::chrono::steady_clock::now();
    if (s.gaveWay && now - *s.gaveWay < kSettle) return Shortfall::GivingWay;
    s.gaveWay = now;
    log::warn("engine: this computer cannot keep up with %zu engines playing; the one started last among those upsampling "
              "pauses",
              s.players.size());
    if (last->giveWay) last->giveWay();
    return Shortfall::GivingWay;
}

} // namespace camel::audio::load
