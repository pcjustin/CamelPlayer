// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/EngineGroup.h"

#include <algorithm>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::audio {

namespace {

constexpr auto kResyncEvery = std::chrono::seconds(2);
constexpr size_t kGains = 16;

bool isLocal(Backend backend) {
    return backend == Backend::Wasapi || backend == Backend::Asio || backend == Backend::Ks;
}

// A fault of the member's device, after which the others play on without it.
bool deviceFault(ErrorCode code) {
    return code == ErrorCode::DeviceLost || code == ErrorCode::DeviceNotFound || code == ErrorCode::DeviceInUse ||
           code == ErrorCode::DriverError || code == ErrorCode::AccessDenied;
}

} // namespace

EngineGroup::EngineGroup(const Settings& settings, OutputFactory factory, DeviceWatcherFactory watcher)
    : factory_(std::move(factory)), watcher_(std::move(watcher)) {
    Session own;
    own.settings = settings;
    own.engine = makeEngine(own.link, settings);
    sessions_.push_back(std::move(own));
}

EngineGroup::~EngineGroup() = default;

// Every output of this computer plays through a group member's output, which passes its frames on untouched while it
// plays alone.
std::unique_ptr<PlaybackEngine> EngineGroup::makeEngine(const std::shared_ptr<GroupLink>& link, const Settings& settings) const {
    return std::make_unique<PlaybackEngine>(
        settings,
        [factory = factory_, link](const Settings& s) -> std::unique_ptr<IOutput> {
            auto device = factory(s);
            if (!device || !isLocal(s.backend)) return device;
            return groupMemberOutput(std::move(device), link);
        },
        watcher_);
}

// A follower converts DSD to PCM, which its resampler can keep to the clock.
Settings EngineGroup::sessionSettings(const Session& s, bool follower) const {
    Settings settings = s.settings;
    if (follower) {
        settings.wasapi.dsdOutput = DsdOutput::Pcm;
        settings.asio.dsdOutput = DsdOutput::Pcm;
        settings.ks.dsdOutput = DsdOutput::Pcm;
    }
    return settings;
}

void EngineGroup::place() {
    const bool grouped = sessions_.size() > 1;
    const int32_t clockDelay = sessions_[leader_].delayMs;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        auto& link = *sessions_[i].link;
        link.delaySeconds = double(sessions_[i].delayMs - clockDelay) / 1000.0;
        link.follower = grouped && i != leader_;
        link.clock = grouped ? &clock_ : nullptr;
    }
}

void EngineGroup::setMembers(std::vector<Member> members, size_t clock) {
    std::unique_lock lock(mutex_);
    if (members.empty()) return;
    if (clock >= members.size()) clock = 0;
    const EngineSnapshot before = leader().snapshot();
    std::vector<Session> kept;
    std::vector<size_t> joined;
    for (auto& member : members) {
        auto found = std::ranges::find_if(sessions_, [&](const Session& s) { return s.engine && s.key == member.key; });
        // The zone's own session stays its first, whatever its key was before the zone became a group.
        if (found == sessions_.end() && kept.empty()) found = sessions_.begin();
        if (found != sessions_.end() && found->engine) {
            kept.push_back(std::move(*found));
        } else {
            Session s;
            joined.push_back(kept.size());
            kept.push_back(std::move(s));
        }
        Session& s = kept.back();
        s.key = member.key;
        s.delayMs = member.delayMs;
        s.settings = std::move(member.settings);
    }
    for (auto& gone : sessions_)
        if (gone.engine) gone.engine->stop();
    sessions_ = std::move(kept);
    leader_ = clock;
    for (const size_t i : joined) {
        Session& s = sessions_[i];
        s.engine = makeEngine(s.link, sessionSettings(s, sessions_.size() > 1 && i != leader_));
        if (listener_) s.engine->setListener(listener_);
    }
    // A clock that is no longer one plays as a follower from where it is; a follower that becomes the clock, as itself.
    place();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session& s = sessions_[i];
        s.lost.reset();
        s.detail.clear();
        if (std::ranges::find(joined, i) == joined.end())
            s.engine->applySettings(sessionSettings(s, sessions_.size() > 1 && i != leader_));
    }
    for (const size_t i : joined) resync(sessions_[i], before);
}

std::vector<EngineGroup::MemberState> EngineGroup::members() const {
    std::shared_lock lock(mutex_);
    std::vector<MemberState> states;
    if (sessions_.size() < 2) return states;
    for (size_t i = 0; i < sessions_.size(); ++i)
        states.push_back({sessions_[i].key, i == leader_, sessions_[i].lost, sessions_[i].detail});
    return states;
}

// A member started afresh at where the clock is, as it joins or once it no longer plays what the clock plays; its
// resampler takes it the rest of the way.
void EngineGroup::resync(Session& s, const EngineSnapshot& leader) {
    s.resynced = std::chrono::steady_clock::now();
    if (leader.file.empty() || leader.state == PlayState::Stopped) {
        s.engine->stop();
        return;
    }
    s.engine->load(leader.file, std::nullopt, gainOf(leader.file));
    s.engine->seek(leader.position);
    if (leader.state == PlayState::Playing) s.engine->play();
}

void EngineGroup::tend() {
    std::unique_lock lock(mutex_);
    if (sessions_.size() < 2) return;
    auto ls = leader().snapshot();
    // The clock's device failed: the next member that plays takes over as the clock, playing on where it is.
    if (ls.error && deviceFault(*ls.error)) {
        const auto next = std::ranges::find_if(sessions_, [&](const Session& s) { return &s != &sessions_[leader_] && !s.lost; });
        if (next != sessions_.end()) {
            Session& old = sessions_[leader_];
            old.lost = ls.error;
            old.detail = ls.errorDetail;
            old.engine->stop();
            log::warn("zone group: %s failed (%s), the clock passes to %s", old.key.c_str(), ls.errorDetail.c_str(),
                      next->key.c_str());
            leader_ = size_t(next - sessions_.begin());
            place();
            old.link->clock = nullptr;
            ls = leader().snapshot();
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        Session& s = sessions_[i];
        if (i == leader_ || s.lost) continue;
        const auto fs = s.engine->snapshot();
        if (fs.error && deviceFault(*fs.error)) {
            s.lost = fs.error;
            s.detail = fs.errorDetail;
            s.engine->stop();
            s.link->clock = nullptr;
            log::warn("zone group: %s failed (%s), the others play on", s.key.c_str(), fs.errorDetail.c_str());
            continue;
        }
        if (ls.state == PlayState::Playing) {
            // A follower reaches a track's end a moment before the clock, and takes the next one a moment after it.
            const uint32_t rate = ls.source ? ls.source->sampleRate : 0;
            const bool settled = ls.position >= rate / 2 && (!ls.total || ls.position + 2 * uint64_t(rate) < ls.total);
            const bool apart = fs.file != ls.file || fs.state != PlayState::Playing;
            if (apart && settled && now - s.resynced >= kResyncEvery) resync(s, ls);
        } else if (ls.state == PlayState::Paused && fs.state == PlayState::Playing) {
            s.engine->pause();
        } else if (ls.state == PlayState::Stopped && fs.state != PlayState::Stopped) {
            s.engine->stop();
        }
    }
}

template <class F> void EngineGroup::forEach(F&& f) {
    std::shared_lock lock(mutex_);
    f(*sessions_[leader_].engine);
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (i != leader_ && !sessions_[i].lost) f(*sessions_[i].engine);
}

double EngineGroup::gainOf(const std::wstring& file) const {
    const auto found = std::ranges::find(gains_, file, &std::pair<std::wstring, double>::first);
    return found == gains_.end() ? 0.0 : found->second;
}

void EngineGroup::remember(const std::filesystem::path& file, double gainDb) {
    std::unique_lock lock(mutex_);
    std::erase_if(gains_, [&](const auto& g) { return g.first == file.wstring(); });
    gains_.emplace_back(file.wstring(), gainDb);
    if (gains_.size() > kGains) gains_.pop_front();
}

void EngineGroup::openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb,
                              bool afterEnd) {
    remember(file, gainDb);
    forEach([&](PlaybackEngine& e) { e.openAndPlay(file, decoder, gainDb, afterEnd); });
}

void EngineGroup::crossfadeTo(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb) {
    remember(file, gainDb);
    forEach([&](PlaybackEngine& e) { e.crossfadeTo(file, decoder, gainDb); });
}

void EngineGroup::load(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb) {
    remember(file, gainDb);
    forEach([&](PlaybackEngine& e) { e.load(file, decoder, gainDb); });
}

void EngineGroup::play() {
    forEach([](PlaybackEngine& e) { e.play(); });
}

void EngineGroup::pause() {
    forEach([](PlaybackEngine& e) { e.pause(); });
}

// Every member does what the clock does, so none toggles into the opposite state.
void EngineGroup::togglePause() {
    const bool playing = snapshot().state == PlayState::Playing;
    if (playing)
        pause();
    else
        play();
}

void EngineGroup::stop() {
    forEach([](PlaybackEngine& e) { e.stop(); });
}

void EngineGroup::seek(uint64_t frame) {
    forEach([&](PlaybackEngine& e) { e.seek(frame); });
}

void EngineGroup::applySettings(const Settings& settings) {
    std::unique_lock lock(mutex_);
    Session& own = sessions_.front();
    own.settings = settings;
    own.engine->applySettings(sessionSettings(own, sessions_.size() > 1 && leader_ != 0));
}

bool EngineGroup::showControlPanel() {
    std::shared_lock lock(mutex_);
    return sessions_.front().engine->showControlPanel();
}

bool EngineGroup::queueNext(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb,
                            uint32_t crossfadeMs) {
    remember(file, gainDb);
    std::shared_lock lock(mutex_);
    const bool queued = leader().queueNext(file, decoder, gainDb, crossfadeMs);
    if (queued)
        for (size_t i = 0; i < sessions_.size(); ++i)
            if (i != leader_ && !sessions_[i].lost) sessions_[i].engine->queueNext(file, decoder, gainDb, crossfadeMs);
    return queued;
}

bool EngineGroup::clearNext() {
    std::shared_lock lock(mutex_);
    const bool cleared = leader().clearNext();
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (i != leader_ && !sessions_[i].lost) sessions_[i].engine->clearNext();
    return cleared;
}

void EngineGroup::discardPreloads() {
    forEach([](PlaybackEngine& e) { e.discardPreloads(); });
}

EngineSnapshot EngineGroup::snapshot() const {
    std::shared_lock lock(mutex_);
    return leader().snapshot();
}

void EngineGroup::setListener(std::function<void()> listener) {
    std::unique_lock lock(mutex_);
    listener_ = std::move(listener);
    for (auto& s : sessions_) s.engine->setListener(listener_);
}

void EngineGroup::waitIdle() {
    forEach([](PlaybackEngine& e) { e.waitIdle(); });
}

void EngineGroup::waitIdle(std::chrono::milliseconds period, const std::function<void()>& meanwhile) {
    forEach([&](PlaybackEngine& e) { e.waitIdle(period, meanwhile); });
}

bool EngineGroup::waitForState(PlayState s, std::chrono::milliseconds timeout) {
    std::shared_lock lock(mutex_);
    return leader().waitForState(s, timeout);
}

} // namespace camel::audio
