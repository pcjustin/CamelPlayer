#include "server/LocalServer.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "util/Log.h"
#include "util/Text.h"

namespace camel::server {
namespace {

SettingsDto dto(const Settings& s) {
    return {s.backend, s.decoder, s.wasapi, s.asio, s.ks, s.language, toUtf8(s.lastFile)};
}

Settings native(const SettingsDto& s) {
    return {s.backend, s.decoder, s.wasapi, s.asio, s.ks, s.language, toWide(s.lastFileUtf8)};
}

bool covers(CommandStamp value, CommandStamp floor) {
    return value.id >= floor.id && value.generation >= floor.generation && value.queueRevision >= floor.queueRevision;
}

bool replaces(CommandKind kind) {
    return kind == CommandKind::Load || kind == CommandKind::OpenAndPlay;
}

void validate(const Command& c) {
    if (c.apiVersion != kApiVersion || c.kind > CommandKind::ShowControlPanel)
        throw std::invalid_argument("unsupported Server command/version");
    if (c.source.has_value() != replaces(c.kind) || c.settings.has_value() != (c.kind == CommandKind::ApplySettings) ||
        (c.frame != 0 && c.kind != CommandKind::Seek))
        throw std::invalid_argument("invalid Server command payload");
    if (c.source && c.source->range() != FrameRange{})
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
    p.output = s.output;
    p.backend = s.backend;
    p.decoder = s.decoder;
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
        if (replaces(c.kind) || c.kind == CommandKind::Stop || c.kind == CommandKind::Seek ||
            c.kind == CommandKind::ApplySettings)
            ++next.generation;
        if (replaces(c.kind)) ++next.queueRevision;
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

    Settings effectiveSettings() const {
        Settings result = settings_;
        if (state_.track) result.decoder = state_.track->decoder().value_or(result.decoder);
        return result;
    }

    void execute(const Pending& pending) {
        const auto& c = pending.command;
        state_.command = pending.stamp;
        state_.commandError.reset();
        state_.controlPanelOpened.reset();
        if (c.source) {
            const std::filesystem::path path(toWide(c.source.value().pathUtf8()));
            if (pending.restore) {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) return;
            }
            state_.track = c.source;
            state_.queueEntryId = pending.stamp.id;
            settings_.lastFile = path.wstring();
            const auto effective = effectiveSettings();
            if (engine_->snapshot().decoder != effective.decoder) {
                engine_->stop();
                engine_->applySettings(effective);
            }
            if (c.kind == CommandKind::Load)
                engine_->load(path);
            else
                engine_->openAndPlay(path);
            persist();
        } else {
            switch (c.kind) {
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
                    engine_->applySettings(effectiveSettings());
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

    void publish(bool completed) {
        const auto p = playback(engine_->snapshot());
        if (!completed && p.revision == state_.playback.revision && p.position == state_.playback.position &&
            p.underruns == state_.playback.underruns)
            return;
        state_.playback = p;
        state_.settings = dto(settings_);
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
