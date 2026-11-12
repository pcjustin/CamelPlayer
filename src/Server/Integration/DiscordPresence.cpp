// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Integration/DiscordPresence.h"

#include <cstdio>

#include "Common/AudioFormat.h"
#include "Common/util/Text.h"

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;

// Text first goes out at most this long after a track change while its cover is on its way.
constexpr auto kCoverWait = std::chrono::seconds(5);

std::string kilohertz(uint32_t rate) {
    char text[32];
    if (rate % 1000 == 0)
        std::snprintf(text, sizeof(text), "%u kHz", rate / 1000);
    else
        std::snprintf(text, sizeof(text), "%.1f kHz", rate / 1000.0);
    return text;
}

const std::string& stateText(audio::PipelineState state, const DiscordTexts& texts) {
    switch (state) {
    case audio::PipelineState::BitPerfect:
        return texts.bitPerfect;
    case audio::PipelineState::LosslessConversion:
        return texts.lossless;
    case audio::PipelineState::Processed:
        return texts.processed;
    case audio::PipelineState::LossySource:
        break;
    }
    return texts.lossy;
}

const char* stateImage(audio::PipelineState state) {
    switch (state) {
    case audio::PipelineState::BitPerfect:
        return "bitperfect";
    case audio::PipelineState::LosslessConversion:
        return "lossless";
    case audio::PipelineState::Processed:
        return "processed";
    case audio::PipelineState::LossySource:
        break;
    }
    return "lossy";
}

} // namespace

std::string discordFormat(const PlaybackSnapshot& playback) {
    if (!playback.format || !playback.source) return {};
    const auto& source = *playback.source;
    const std::string name = audio::formatName(*playback.format);
    // The decoder counts DSD in byte frames of eight samples.
    if (*playback.format == audio::AudioFormat::Dsd) return "DSD" + std::to_string(source.sampleRate * 8 / 44100);
    if (audio::isLossy(*playback.format)) return name + " " + kilohertz(source.sampleRate);
    return name + " " + std::to_string(source.bitsPerSample) + "-bit " + kilohertz(source.sampleRate);
}

std::optional<DiscordActivity> discordActivity(const Snapshot& snapshot, const DiscordTexts& texts, int64_t nowMs) {
    const auto& playback = snapshot.playback;
    if (playback.state != PlaybackState::Playing || playback.pathUtf8.empty()) return std::nullopt;
    DiscordActivity activity;
    for (const auto& entry : snapshot.queue.entries)
        if (entry.id == snapshot.queueEntryId) {
            activity.title = entry.title;
            activity.artists = entry.artist;
            activity.album = entry.album;
            break;
        }
    if (activity.title.empty()) activity.title = toUtf8(std::filesystem::path(toWide(playback.pathUtf8)).stem().wstring());
    const uint32_t rate = playback.source ? playback.source->sampleRate : 0;
    const auto position = int64_t(audio::framesToSeconds(playback.position, rate) * 1000.0);
    activity.startMs = nowMs - position;
    if (rate && playback.total)
        activity.endMs = activity.startMs + int64_t(audio::framesToSeconds(playback.total, rate) * 1000.0);
    activity.smallText = discordFormat(playback);
    if (playback.plan) {
        activity.smallImage = stateImage(playback.plan->state);
        const auto& state = stateText(playback.plan->state, texts);
        activity.smallText = activity.smallText.empty() ? state : activity.smallText + " · " + state;
    }
    return activity;
}

DiscordPresence::DiscordPresence(std::filesystem::path cloudflared, CoverLoader loader, DiscordClient::Options client,
                                 CoverTunnel::Options tunnel)
    : loader_(std::move(loader)), tunnelOptions_(std::move(tunnel)), client_(std::move(client)),
      coverWorker_([this](const std::stop_token& stop) { loadCovers(stop); }) {
    tunnelOptions_.executable = std::move(cloudflared);
}

DiscordPresence::~DiscordPresence() {
    coverCancel_.cancel();
    coverWorker_ = {};
}

void DiscordPresence::configure(const DiscordSettings& settings) {
    std::lock_guard lock(mutex_);
    if (settings_ == settings) return;
    settings_ = settings;
    client_.setEnabled(settings.enabled);
    const bool covers = settings.enabled && settings.covers && !tunnelOptions_.executable.empty();
    if (covers && !tunnel_) tunnel_ = std::make_unique<CoverTunnel>(tunnelOptions_);
    if (tunnel_) tunnel_->setEnabled(covers);
    // The next snapshot picks the track up again, and its cover with it.
    track_.clear();
    coverFor_.clear();
    servedFor_.clear();
}

void DiscordPresence::setTexts(const DiscordTexts& texts) {
    std::lock_guard lock(mutex_);
    texts_ = texts;
}

void DiscordPresence::observe(const Snapshot& snapshot) {
    std::lock_guard lock(mutex_);
    if (!settings_.enabled) return;
    const auto nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto activity = discordActivity(snapshot, texts_, nowMs);
    const bool covers = tunnel_ && settings_.covers;
    if (covers) tunnel_->setPlaying(activity.has_value());
    if (!activity) {
        client_.show(std::nullopt);
        return;
    }
    const auto& path = snapshot.playback.pathUtf8;
    const std::string track = std::to_string(snapshot.queueEntryId) + "|" + path;
    if (track != track_) {
        track_ = track;
        trackSince_ = Clock::now();
        if (covers && coverFor_ != path) {
            coverCancel_.cancel();
            coverCancel_ = {};
            coverFor_ = path;
            ++coverAsked_;
            coverWanted_.notify_all();
        }
    }
    if (covers) {
        const std::string url = servedFor_ == path ? tunnel_->url() : std::string();
        // Up to kCoverWait the text waits for the cover, so a track change reaches Discord once.
        const bool coming =
            servedFor_ != path || (servedCover_ && url.empty() && tunnel_->status().state != CoverTunnel::State::Failed);
        if (coming && Clock::now() - trackSince_ < kCoverWait) return;
        activity->largeImage = url;
    }
    client_.show(activity);
}

DiscordStatus DiscordPresence::status() const {
    std::lock_guard lock(mutex_);
    DiscordStatus status;
    if (!settings_.enabled) return status;
    switch (client_.state()) {
    case DiscordClient::State::Off:
        break;
    case DiscordClient::State::Searching:
        status.connection = DiscordConnection::Searching;
        break;
    case DiscordClient::State::Connected:
        status.connection = DiscordConnection::Connected;
        break;
    }
    if (tunnel_ && settings_.covers) {
        const auto tunnel = tunnel_->status();
        status.cover = DiscordCover(tunnel.state);
        status.coverError = tunnel.error;
    }
    return status;
}

void DiscordPresence::loadCovers(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    uint64_t loaded = 0;
    while (!stop.stop_requested()) {
        coverWanted_.wait(lock, stop, [&] { return coverAsked_ != loaded; });
        if (stop.stop_requested()) break;
        const uint64_t asked = coverAsked_;
        const std::string path = coverFor_;
        const auto token = coverCancel_;
        lock.unlock();
        std::vector<uint8_t> jpeg;
        try {
            jpeg = loader_(path, token);
        } catch (const std::exception&) {
        }
        lock.lock();
        loaded = asked;
        if (token.cancelled() || coverAsked_ != asked || !tunnel_) continue;
        servedCover_ = !jpeg.empty();
        tunnel_->serve(std::move(jpeg));
        servedFor_ = path;
    }
}

} // namespace camel::server
