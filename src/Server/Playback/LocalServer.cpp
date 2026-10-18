#include "Server/Playback/LocalServer.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::server {
namespace {

SettingsDto dto(const Settings& s) {
    return {s.backend, s.decoderByFormat, s.pcmBitsByFormat, s.wasapi, s.asio, s.ks, s.language, toUtf8(s.lastFile)};
}

Settings native(const SettingsDto& s) {
    return {s.backend, s.decoderByFormat, s.pcmBitsByFormat, s.wasapi, s.asio, s.ks, s.language, toWide(s.lastFileUtf8)};
}

bool covers(CommandStamp value, CommandStamp floor) {
    return value.id >= floor.id && value.generation >= floor.generation && value.queueRevision >= floor.queueRevision;
}

bool replaces(CommandKind kind) {
    return kind == CommandKind::Load || kind == CommandKind::OpenAndPlay;
}

bool startsEntry(CommandKind kind) {
    return replaces(kind) || kind == CommandKind::QueueReplace || kind == CommandKind::QueueSelect || kind == CommandKind::Next ||
           kind == CommandKind::Previous;
}

bool changesQueue(CommandKind kind) {
    return replaces(kind) || kind == CommandKind::QueueReplace || kind == CommandKind::QueueReorder ||
           kind == CommandKind::SetShuffle;
}

// Problems of one track rather than of the device: report them and move on to the next eligible entry.
bool trackLocal(audio::ErrorCode code) {
    switch (code) {
    case audio::ErrorCode::FileOpen:
    case audio::ErrorCode::UnsupportedStream:
    case audio::ErrorCode::DecodeFailed:
    case audio::ErrorCode::SeekFailed:
    case audio::ErrorCode::FormatNotSupported:
        return true;
    default:
        return false;
    }
}

void validate(const Command& c) {
    if (c.apiVersion != kApiVersion || c.kind > CommandKind::SetRepeat)
        throw std::invalid_argument("unsupported Server command/version");
    const bool queueReplace = c.kind == CommandKind::QueueReplace;
    if (c.source.has_value() != replaces(c.kind) || c.settings.has_value() != (c.kind == CommandKind::ApplySettings) ||
        (c.frame != 0 && c.kind != CommandKind::Seek) || c.sources.empty() == queueReplace ||
        (queueReplace ? c.index >= c.sources.size() : c.index != 0) ||
        c.entryIds.empty() == (c.kind == CommandKind::QueueReorder) || (c.entryId != 0) != (c.kind == CommandKind::QueueSelect) ||
        c.shuffle.has_value() != (c.kind == CommandKind::SetShuffle) ||
        c.repeat.has_value() != (c.kind == CommandKind::SetRepeat) || (c.repeat && *c.repeat > RepeatMode::All))
        throw std::invalid_argument("invalid Server command payload");
    const auto ranged = [](const TrackSource& source) { return source.range() != FrameRange{}; };
    if ((c.source && ranged(*c.source)) || std::any_of(c.sources.begin(), c.sources.end(), ranged))
        throw std::invalid_argument("track ranges require the future segment playback adapter");
    if (c.settings && dto(fromJson(toJson(native(*c.settings)))) != *c.settings)
        throw std::invalid_argument("invalid Server settings");
}

PlaybackSnapshot playback(const audio::EngineSnapshot& s) {
    PlaybackSnapshot p;
    switch (s.state) {
    case audio::PlayState::Stopped:
        p.state = PlaybackState::Stopped;
        break;
    case audio::PlayState::Playing:
        p.state = PlaybackState::Playing;
        break;
    case audio::PlayState::Paused:
        p.state = PlaybackState::Paused;
        break;
    }
    p.pathUtf8 = toUtf8(s.file);
    p.position = s.position;
    p.total = s.total;
    p.source = s.source;
    p.format = s.format;
    p.output = s.output;
    p.backend = s.backend;
    p.decoder = s.decoder;
    if (s.output) {
        p.activeBackend = s.backend;
        p.activeDecoder = s.decoder;
    }
    p.outputMode = s.outputStatus.mode;
    p.outputDetail = s.outputStatus.detail;
    p.bufferFrames = s.outputStatus.bufferFrames;
    p.underruns = s.underruns;
    p.error = s.error;
    p.errorDetail = s.errorDetail;
    p.revision = s.revision;
    return p;
}

std::vector<DeviceInfo> devices(const std::vector<audio::DeviceEntry>& entries) {
    std::vector<DeviceInfo> result;
    result.reserve(entries.size());
    for (const auto& e : entries) result.push_back({e.id, e.name});
    return result;
}

} // namespace

TrackSource::TrackSource(std::string pathUtf8, FrameRange range, std::optional<audio::DecoderKind> decoder)
    : pathUtf8_(std::move(pathUtf8)), range_(range), decoder_(decoder) {
    if (pathUtf8_.empty() || pathUtf8_.find('\0') != std::string::npos || toUtf8(toWide(pathUtf8_)) != pathUtf8_)
        throw std::invalid_argument("TrackSource requires a nonempty UTF-8 path");
    if (range_.endExclusive && *range_.endExclusive <= range_.first)
        throw std::invalid_argument("TrackSource range must be nonempty");
    if (decoder_ &&
        std::find(std::begin(audio::kAllDecoders), std::end(audio::kAllDecoders), *decoder_) == std::end(audio::kAllDecoders))
        throw std::invalid_argument("invalid TrackSource decoder");
}

void SnapshotCursor::expect(CommandStamp command) {
    expected_.id = std::max(expected_.id, command.id);
    expected_.generation = std::max(expected_.generation, command.generation);
    expected_.queueRevision = std::max(expected_.queueRevision, command.queueRevision);
}

bool SnapshotCursor::accept(const Snapshot& s) {
    if (s.apiVersion != kApiVersion || !covers(s.command, expected_) ||
        (current_ && (s.revision <= current_->revision || !covers(s.command, current_->command))))
        return false;
    current_ = s;
    return true;
}

class LocalServer::Impl {
public:
    Impl(const Settings& settings, std::filesystem::path settingsFile, audio::OutputFactory factory)
        : settings_(settings), settingsFile_(std::move(settingsFile)),
          engine_(std::make_unique<audio::PlaybackEngine>(settings, std::move(factory))) {
        published_.settings = dto(settings_);
        published_.playback = playback(engine_->snapshot());
        if (!settings_.lastFile.empty()) {
            Command restore;
            restore.kind = CommandKind::Load;
            restore.source.emplace(toUtf8(settings_.lastFile));
            enqueue(std::move(restore), true);
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            listener_ = {};
        }
        cv_.notify_one();
        worker_.join();
    }

    CommandStamp enqueue(Command c, bool restore = false) {
        validate(c);
        std::lock_guard lock(mutex_);
        if (closing_) throw std::runtime_error("Server is closing");
        CommandStamp next = issued_;
        ++next.id;
        if (startsEntry(c.kind) || c.kind == CommandKind::Stop || c.kind == CommandKind::Seek ||
            c.kind == CommandKind::ApplySettings)
            ++next.generation;
        if (changesQueue(c.kind)) ++next.queueRevision;
        queue_.push_back({std::move(c), next, restore});
        issued_ = next;
        cv_.notify_one();
        return next;
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return published_;
    }

    void setListener(std::function<void(const Event&)> listener) {
        std::lock_guard lock(mutex_);
        listener_ = std::move(listener);
    }

    void waitIdle() {
        std::unique_lock lock(mutex_);
        const auto target = issued_.id;
        idle_.wait(lock, [&] { return processed_ >= target; });
    }

private:
    struct Pending {
        Command command;
        CommandStamp stamp;
        bool restore;
    };

    void persist() {
        if (settingsFile_.empty()) return;
        try {
            saveSettings(settings_, settingsFile_);
        } catch (const std::exception& e) {
            state_.commandError = Failure{audio::ErrorCode::Other, e.what()};
        }
    }

    void execute(const Pending& pending) {
        const auto& c = pending.command;
        state_.command = pending.stamp;
        attempted_ = {}; // a command may restart the stream, so the upcoming entry may be queued again
        state_.commandError.reset();
        state_.controlPanelOpened.reset();
        if (c.source) {
            if (pending.restore) {
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::path(toWide(c.source->pathUtf8())), ec)) return;
            }
            const auto ids = replaceQueue({*c.source});
            startEntry(policy_.select(ids.front()), c.kind == CommandKind::OpenAndPlay);
        } else {
            switch (c.kind) {
            case CommandKind::QueueReplace: {
                const auto ids = replaceQueue(c.sources);
                startEntry(policy_.select(ids[size_t(c.index)]), true);
                break;
            }
            case CommandKind::QueueReorder:
                reorderQueue(c.entryIds);
                break;
            case CommandKind::QueueSelect:
                startEntry(policy_.select(c.entryId), true);
                break;
            case CommandKind::Next:
                if (const auto next = policy_.next(false))
                    startEntry(*next, true);
                else
                    engine_->stop();
                break;
            case CommandKind::Previous: {
                const auto playback = engine_->snapshot();
                const auto previous = policy_.previous(playback.position, playback.source ? playback.source->sampleRate : 0);
                if (!previous) break;
                if (previous->entryId == state_.queueEntryId)
                    engine_->seek(0);
                else
                    startEntry(*previous, true);
                break;
            }
            case CommandKind::SetShuffle:
                if (c.shuffle) policy_.setShuffle(*c.shuffle);
                break;
            case CommandKind::SetRepeat:
                if (c.repeat) policy_.setRepeat(*c.repeat);
                break;
            case CommandKind::Play:
                engine_->play();
                break;
            case CommandKind::Pause:
                engine_->pause();
                break;
            case CommandKind::TogglePause:
                engine_->togglePause();
                break;
            case CommandKind::Stop:
                engine_->stop();
                break;
            case CommandKind::Seek:
                engine_->seek(c.frame);
                break;
            case CommandKind::ApplySettings: {
                if (c.settings) {
                    const auto lastFile = settings_.lastFile;
                    settings_ = native(*c.settings);
                    settings_.lastFile = lastFile;
                    engine_->applySettings(settings_);
                    persist();
                }
                break;
            }
            case CommandKind::RefreshDevices: {
                DeviceCatalog catalog;
                catalog.wasapi = devices(audio::listWasapiDevices());
                catalog.asio = devices(audio::listAsioDrivers());
                catalog.ks = devices(audio::listKsPins());
                catalog.revision = state_.devices.revision + 1;
                state_.devices = std::move(catalog);
                break;
            }
            case CommandKind::ShowControlPanel:
                state_.controlPanelOpened = engine_->showControlPanel();
                break;
            default:
                break;
            }
        }
        engine_->waitIdle();
    }

    std::vector<uint64_t> replaceQueue(const std::vector<TrackSource>& sources) {
        std::vector<uint64_t> ids;
        std::vector<QueueEntry> entries;
        for (const auto& source : sources) {
            ids.push_back(++lastEntryId_);
            entries.push_back({ids.back(), source});
        }
        policy_.replace(ids);
        entries_ = std::move(entries);
        return ids;
    }

    void reorderQueue(const std::vector<uint64_t>& ids) {
        policy_.reorder(ids);
        std::vector<QueueEntry> entries;
        entries.reserve(ids.size());
        for (const auto id : ids) entries.push_back(entry(id));
        entries_ = std::move(entries);
    }

    const QueueEntry& entry(uint64_t id) const {
        return *std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == id; });
    }

    // Ends of the previous entry that are still pending belong to playback this call supersedes.
    void startEntry(const QueuePosition& position, bool play) {
        engine_->waitIdle();
        const auto before = engine_->snapshot();
        seenTrackEnds_ = before.trackEnds;
        seenTransitions_ = before.transitions;
        chain_.clear();
        const auto& source = entry(position.entryId).source;
        const std::filesystem::path path(toWide(source.pathUtf8()));
        state_.track = source;
        state_.queueEntryId = position.entryId;
        settings_.lastFile = path.wstring();
        if (play)
            engine_->openAndPlay(path, source.decoder());
        else
            engine_->load(path, source.decoder());
        engine_->waitIdle(); // the next snapshot pairs this entry with its own playback state
        persist();
    }

    // A gapless transition already plays the queued entry; it becomes current without reopening anything.
    void commitTransition() {
        const auto playback = engine_->snapshot();
        if (playback.transitions == seenTransitions_) return;
        auto heard = playback.transitions - seenTransitions_;
        seenTransitions_ = playback.transitions;
        for (; heard && !chain_.empty(); --heard) {
            const auto id = chain_.front();
            chain_.pop_front();
            auto position = policy_.next(true);
            if (!position || position->entryId != id) position = policy_.select(id);
            const auto& source = entry(position->entryId).source;
            state_.track = source;
            state_.queueEntryId = position->entryId;
            settings_.lastFile = toWide(source.pathUtf8());
            queueChanged_ = true;
        }
        persist();
    }

    // Keeps the entries that follow queued in the engine while a stream is open, so they play gaplessly. The engine
    // takes one more as soon as its decoder moves on to the previous one, seconds before that one is heard.
    void ensureNextQueued() {
        const auto playback = engine_->snapshot();
        if (playback.transitions != seenTransitions_) return; // committed on the next pass
        if (playback.queuedFiles == 0) chain_.clear();        // a restarted stream forgets queued files
        if (!playback.output || !state_.queueEntryId || !state_.track) return;
        auto copy = policy_;
        for (size_t i = 0; i < chain_.size(); ++i) {
            const auto predicted = copy.next(true);
            if (predicted && predicted->entryId == chain_[i]) continue;
            // The order changed: withdraw a file still waiting in the slot; one already decoding plays regardless.
            if (i + 1 == chain_.size() && !playback.nextSlotFree && engine_->clearNext()) {
                chain_.pop_back();
                return;
            }
            copy.select(chain_[i]);
        }
        if (!playback.nextSlotFree || chain_.size() >= 2) return;
        const auto upcoming = copy.upcoming();
        if (!upcoming) return;
        const QueueAttempt attempt{state_.queueEntryId, upcoming->entryId, policy_.revision(), chain_.size()};
        if (attempt == attempted_) return;
        attempted_ = attempt;
        const auto& source = entry(upcoming->entryId).source;
        if (source.decoder() != state_.track->decoder()) return;
        if (engine_->queueNext(std::filesystem::path(toWide(source.pathUtf8())), source.decoder()))
            chain_.push_back(upcoming->entryId);
    }

    // Natural completion plays the next eligible entry; a track-local failure marks the entry and skips it.
    void advanceAfterTrackEnd() {
        const auto playback = engine_->snapshot();
        if (playback.trackEnds == seenTrackEnds_) return;
        seenTrackEnds_ = playback.trackEnds;
        if (!state_.queueEntryId) return;
        if (playback.error) {
            if (!trackLocal(*playback.error)) return;
            policy_.markFailed(state_.queueEntryId);
            queueChanged_ = true;
        }
        if (const auto next = policy_.next(true)) startEntry(*next, true);
    }

    QueueSnapshot queueSnapshot() const {
        QueueSnapshot queue;
        queue.revision = policy_.revision();
        queue.entries = entries_;
        if (const auto upcoming = policy_.upcoming()) queue.upcomingEntryId = upcoming->entryId;
        queue.shuffle = policy_.shuffle();
        queue.repeat = policy_.repeat();
        return queue;
    }

    void publish(bool completed) {
        const auto p = playback(engine_->snapshot());
        if (!completed && !queueChanged_ && p.revision == state_.playback.revision && p.position == state_.playback.position &&
            p.underruns == state_.playback.underruns)
            return;
        state_.playback = p;
        state_.settings = dto(settings_);
        state_.queue = queueSnapshot();
        queueChanged_ = false;
        Event event;
        std::function<void(const Event&)> listener;
        {
            std::lock_guard lock(mutex_);
            if (state_.command.generation != issued_.generation || state_.command.queueRevision != issued_.queueRevision) return;
            state_.revision = published_.revision + 1;
            published_ = state_;
            event = {completed ? EventKind::CommandCompleted : EventKind::StateChanged, published_};
            listener = listener_;
        }
        if (listener) {
            try {
                listener(event);
            } catch (...) {
                log::warn("Server listener failed");
            }
        }
    }

    void run() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::optional<Pending> pending;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return closing_ || !queue_.empty(); });
                if (!queue_.empty()) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                } else if (closing_)
                    break;
            }
            if (pending) {
                try {
                    execute(*pending);
                } catch (const audio::Error& e) {
                    state_.commandError = Failure{e.code(), e.what()};
                } catch (const std::exception& e) {
                    state_.commandError = Failure{audio::ErrorCode::Other, e.what()};
                } catch (...) {
                    state_.commandError = Failure{audio::ErrorCode::Other, "unknown Server command failure"};
                }
            }
            try {
                commitTransition();
                advanceAfterTrackEnd();
                ensureNextQueued();
            } catch (const std::exception& e) {
                log::error("Server queue advance failed: %s", e.what());
            }
            publish(pending.has_value());
            if (pending) {
                {
                    std::lock_guard lock(mutex_);
                    processed_ = pending->stamp.id;
                }
                idle_.notify_all();
            }
        }
        engine_.reset();
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    Settings settings_;
    std::filesystem::path settingsFile_;
    std::unique_ptr<audio::PlaybackEngine> engine_;
    mutable std::mutex mutex_;
    std::condition_variable cv_, idle_;
    std::deque<Pending> queue_;
    CommandStamp issued_;
    uint64_t processed_ = 0;
    bool closing_ = false;
    Snapshot published_, state_;
    QueuePolicy policy_;
    std::vector<QueueEntry> entries_;
    uint64_t lastEntryId_ = 0;
    uint64_t seenTrackEnds_ = 0;
    bool queueChanged_ = false; // queue state changed without a command or playback change
    // Gapless: the entry queued in the engine, and the last situation already tried so it is not retried every poll.
    struct QueueAttempt {
        uint64_t current = 0, upcoming = 0, revision = 0;
        size_t depth = 0;
        bool operator==(const QueueAttempt&) const = default;
    };
    uint64_t seenTransitions_ = 0;
    std::deque<uint64_t> chain_; // entries queued in the engine, in playback order
    QueueAttempt attempted_;
    std::function<void(const Event&)> listener_;
    std::thread worker_;
};

LocalServer::LocalServer(const Settings& settings, std::filesystem::path settingsFile, audio::OutputFactory factory)
    : impl_(std::make_unique<Impl>(settings, std::move(settingsFile), std::move(factory))) {}
LocalServer::~LocalServer() = default;
CommandStamp LocalServer::submit(Command command) {
    return impl_->enqueue(std::move(command));
}
Snapshot LocalServer::snapshot() const {
    return impl_->snapshot();
}
void LocalServer::setListener(std::function<void(const Event&)> listener) {
    impl_->setListener(std::move(listener));
}
void LocalServer::waitIdle() {
    impl_->waitIdle();
}

std::shared_ptr<IServer> createLocalServer() {
    const auto path = settingsPath();
    return std::make_shared<LocalServer>(loadSettings(path), path);
}

} // namespace camel::server
