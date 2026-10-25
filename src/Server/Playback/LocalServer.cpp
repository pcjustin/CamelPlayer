// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/LocalServer.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <map>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryText.h"
#include "Server/Library/PlayStatistics.h"
#include "Server/Library/Playlists.h"

namespace camel::server {
namespace {

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
           kind == CommandKind::QueueAdd || kind == CommandKind::QueuePlayNext || kind == CommandKind::QueueRemove ||
           kind == CommandKind::QueueClear || kind == CommandKind::SetShuffle || kind == CommandKind::SetRepeat ||
           kind == CommandKind::RemoveLibraryRoot;
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
    if (c.apiVersion != kApiVersion || c.kind > CommandKind::QueueClear)
        throw std::invalid_argument("unsupported Server command/version");
    const bool queueReplace = c.kind == CommandKind::QueueReplace;
    const bool queueSources = queueReplace || c.kind == CommandKind::QueueAdd || c.kind == CommandKind::QueuePlayNext;
    const bool queueIds = c.kind == CommandKind::QueueReorder || c.kind == CommandKind::QueueRemove;
    if (c.source.has_value() != replaces(c.kind) || c.settings.has_value() != (c.kind == CommandKind::ApplySettings) ||
        (c.frame != 0 && c.kind != CommandKind::Seek) || c.sources.empty() == queueSources ||
        (queueReplace ? c.index >= c.sources.size() : c.index != 0) ||
        c.entryIds.empty() == queueIds || (c.entryId != 0) != (c.kind == CommandKind::QueueSelect) ||
        c.shuffle.has_value() != (c.kind == CommandKind::SetShuffle) ||
        c.repeat.has_value() != (c.kind == CommandKind::SetRepeat) || (c.repeat && *c.repeat > RepeatMode::All) ||
        (c.kind == CommandKind::RemoveLibraryRoot ? c.rootId <= 0 : c.rootId != 0) ||
        c.tracks.empty() == (c.kind == CommandKind::ResetPlayStatistics) || c.path.empty() == (c.kind == CommandKind::AddLibraryRoot))
        throw std::invalid_argument("invalid Server command payload");
    const auto ranged = [](const TrackSource& source) { return source.range() != FrameRange{}; };
    if ((c.source && ranged(*c.source)) || std::any_of(c.sources.begin(), c.sources.end(), ranged))
        throw std::invalid_argument("track ranges require the future segment playback adapter");
    if (c.settings && settingsDto(fromJson(toJson(nativeSettings(*c.settings)))) != *c.settings)
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
    p.plan = s.plan;
    p.md5 = s.md5;
    p.backend = s.backend;
    p.decoder = s.decoder;
    if (s.output) {
        p.activeBackend = s.backend;
        p.activeDecoder = s.decoder;
    }
    p.outputMode = s.outputStatus.mode;
    p.outputDetail = s.outputStatus.detail;
    p.outputAdjustments = s.outputStatus.adjustments;
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
        published_.settings = settingsDto(settings_);
        published_.playback = playback(engine_->snapshot());
        const auto directory = settingsFile_.empty() ? std::filesystem::path{} : std::filesystem::absolute(settingsFile_).parent_path();
        if (!directory.empty() && std::filesystem::is_directory(directory)) {
            artwork_ = std::make_unique<ArtworkCache>(directory / "thumbs");
            library_ = std::make_unique<LibraryStore>(directory);
            libraryText_ = std::make_unique<LibraryTextIndex>(*library_);
            LibraryMonitor::Options options;
            options.playingSource = [this] {
                const auto snapshot = engine_->snapshot();
                return snapshot.state == audio::PlayState::Playing ? std::filesystem::path(snapshot.file)
                                                                  : std::filesystem::path{};
            };
            libraryMonitor_ = std::make_unique<LibraryMonitor>(*library_, std::move(options));
        }
        if (!settings_.lastFile.empty()) {
            Command restore;
            restore.kind = CommandKind::Load;
            restore.source.emplace(toUtf8(settings_.lastFile));
            enqueue(std::move(restore), true);
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() {
        libraryMonitor_.reset();
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

    LibraryTrackPage browseLibrary(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                   const CancellationToken& token) const {
        if (!library_) throw std::runtime_error("library is not configured");
        return camel::server::browseTracks(*library_, offset, limit, revision, token);
    }

    LibraryTrackPage searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token) const {
        if (!library_) throw std::runtime_error("library is not configured");
        return camel::server::searchTracks(*library_, std::move(query), offset, limit, revision, token);
    }

    LibrarySearchResults searchLibrary(std::string query, int64_t offset, int64_t limit,
                                       std::optional<uint64_t> revision, const CancellationToken& token) const {
        if (!library_) throw std::runtime_error("library is not configured");
        return camel::server::searchLibrary(*library_, std::move(query), offset, limit, revision, token);
    }

    PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token) const {
        return camel::server::playStatistics(library(), query, token);
    }

    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token) const {
        return artwork_ ? artwork_->get(library_.get(), request, token) : std::nullopt;
    }

    LibraryStore& library() const {
        if (!library_) throw std::runtime_error("library is not configured");
        return *library_;
    }

    // The published queue keeps every entry: library files keep their identity, others only their path.
    PlaylistChange saveQueueAsPlaylist(std::string name) {
        auto& store = library();
        const auto queue = snapshot().queue;
        auto reader = store.openReader();
        auto file = reader.prepare("SELECT id FROM files WHERE path=? AND available=1");
        std::vector<PlaylistEntry> entries;
        for (const auto& entry : queue.entries) {
            PlaylistEntry item{std::nullopt, entry.source.pathUtf8()};
            try {
                const auto key = toUtf8(canonicalLibraryPath(std::filesystem::path(toWide(entry.source.pathUtf8()))).wstring());
                file.reset();
                if (file.bind(1, key).step()) item.track = LibraryTrackKey{file.integer(0), 0};
            } catch (const std::exception&) {
                // An inaccessible path is saved without a library identity.
            }
            entries.push_back(std::move(item));
        }
        return savePlaylist(store, std::move(name), entries);
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

    void resumePlayback() {
        const auto current = policy_.current();
        if (engine_->snapshot().state == audio::PlayState::Stopped && !entries_.empty() &&
            (!current || policy_.unavailable(current->entryId))) {
            const auto next = policy_.next(false);
            if (!next) throw std::invalid_argument("queue entries are unavailable");
            startEntry(*next, true);
        } else engine_->play();
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
                const auto ids = replaceQueue(c.sources, size_t(c.index));
                startEntry(policy_.select(ids[size_t(c.index)]), true);
                break;
            }
            case CommandKind::QueueReorder:
                reorderQueue(c.entryIds);
                break;
            case CommandKind::QueueAdd:
                appendQueue(c.sources, false);
                break;
            case CommandKind::QueuePlayNext:
                appendQueue(c.sources, true);
                break;
            case CommandKind::QueueRemove:
                removeQueue(c.entryIds);
                break;
            case CommandKind::QueueClear:
                clearQueue();
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
            case CommandKind::AddLibraryRoot:
                if (!library_) throw std::logic_error("Server library is not configured");
                LibraryScanner(*library_).addRoot(std::filesystem::path(toWide(c.path)));
                libraryMonitor_->refreshRoots();
                break;
            case CommandKind::RemoveLibraryRoot:
                if (!library_) throw std::logic_error("Server library is not configured");
                LibraryScanner(*library_).removeRoot(c.rootId);
                refreshLibraryAvailability();
                break;
            case CommandKind::Play:
                resumePlayback();
                break;
            case CommandKind::Pause:
                engine_->pause();
                break;
            case CommandKind::TogglePause:
                if (engine_->snapshot().state == audio::PlayState::Stopped) resumePlayback();
                else engine_->togglePause();
                break;
            case CommandKind::Stop:
                engine_->stop();
                break;
            case CommandKind::Seek:
                if (session_) session_->interrupt();
                engine_->seek(c.frame);
                break;
            case CommandKind::ResetPlayStatistics:
            case CommandKind::ResetAllPlayStatistics:
                if (!library_) throw std::logic_error("Server library is not configured");
                if (session_ && (c.tracks.empty() || std::ranges::find(c.tracks, session_->track()) != c.tracks.end()))
                    session_->discard();
                resetPlayStatistics(*library_, c.tracks);
                break;
            case CommandKind::ApplySettings: {
                if (c.settings) {
                    const auto lastFile = settings_.lastFile;
                    settings_ = nativeSettings(*c.settings);
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

    std::vector<uint64_t> replaceQueue(const std::vector<TrackSource>& sources, size_t selected = 0) {
        libraryEntries_.clear();
        auto entries = buildQueueEntries(sources);
        std::vector<uint64_t> ids;
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.push_back(e.id);
        policy_.replace(ids);
        entries_ = std::move(entries);
        libraryVersion_ = 0;
        refreshLibraryAvailability();
        if (policy_.unavailable(ids[selected])) {
            engine_->stop();
            engine_->waitIdle();
            state_.queueEntryId = 0;
            state_.track.reset();
            chain_.clear();
            throw std::invalid_argument("queue entry is unavailable");
        }
        return ids;
    }

    std::vector<QueueEntry> buildQueueEntries(const std::vector<TrackSource>& sources) {
        std::vector<QueueEntry> entries;
        entries.reserve(sources.size());
        if (library_ && !libraryReader_) libraryReader_.emplace(library_->openReader());
        // shortcut: queue paths resolve serially; batch identity lookup if M3-R07 large-queue latency requires it.
        for (const auto& source : sources) {
            const auto id = ++lastEntryId_;
            entries.push_back({id, source});
            if (libraryReader_) {
                try {
                    auto& binding = libraryEntries_[id];
                    binding.path = canonicalLibraryPath(std::filesystem::path(toWide(source.pathUtf8()))).wstring();
                    auto file = libraryReader_->prepare(
                        "SELECT f.id,coalesce(t.title,''),coalesce(t.artist,''),"
                        "coalesce(nullif(a.title,''),nullif(t.album,''),''),coalesce(a.id,0) FROM files f "
                        "LEFT JOIN library_tracks t ON t.file_id=f.id LEFT JOIN albums a ON a.id=t.album_id "
                        "WHERE f.path=? AND f.available=1 ORDER BY t.cue_index LIMIT 1");
                    if (file.bind(1, toUtf8(binding.path)).step()) {
                        binding.file = file.integer(0);
                        auto& entry = entries.back();
                        entry.title = file.text(1);
                        entry.artist = file.text(2);
                        entry.album = file.text(3);
                        entry.albumId = file.integer(4);
                    }
                } catch (const std::exception& e) {
                    log::warn("library queue source: %s", e.what());
                }
            }
        }
        return entries;
    }

    void appendQueue(const std::vector<TrackSource>& sources, bool playNext) {
        auto entries = buildQueueEntries(sources);
        std::vector<uint64_t> ids;
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.push_back(e.id);
        policy_.append(ids, playNext);
        entries_.insert(entries_.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
        libraryVersion_ = 0;
        refreshLibraryAvailability();
    }

    void removeQueue(const std::vector<uint64_t>& ids) {
        policy_.remove(ids);
        std::erase_if(entries_, [&](const QueueEntry& e) { return std::find(ids.begin(), ids.end(), e.id) != ids.end(); });
        for (const auto id : ids) libraryEntries_.erase(id);
    }

    void clearQueue() {
        const uint64_t current = state_.queueEntryId;
        const auto keep = std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == current; });
        policy_.clearExcept(keep == entries_.end() ? std::nullopt : std::optional(current));
        if (keep == entries_.end()) entries_.clear();
        else {
            QueueEntry currentEntry = *keep;
            entries_ = {std::move(currentEntry)};
        }
        std::erase_if(libraryEntries_, [&](const auto& item) { return item.first != current; });
    }

    void refreshLibraryAvailability() {
        if (!libraryReader_ || libraryEntries_.empty()) return;
        const auto version = libraryReader_->scalar("PRAGMA data_version").value();
        if (version == libraryVersion_) return;
        libraryVersion_ = version;
        std::vector<std::wstring> removed;
        auto roots = libraryReader_->prepare("SELECT path FROM roots WHERE removed IS NOT NULL");
        while (roots.step()) {
            auto path = toWide(roots.text(0));
            if (!path.ends_with(L'\\')) path.push_back(L'\\');
            removed.push_back(std::move(path));
        }
        for (auto& [id, binding] : libraryEntries_) {
            bool unavailable = binding.detached;
            if (binding.file) {
                auto file = libraryReader_->prepare("SELECT f.available,r.online,r.removed FROM files f "
                                                    "JOIN roots r ON r.id=f.root_id WHERE f.id=?");
                unavailable = !file.bind(1, binding.file).step() || !file.integer(0) || !file.integer(1) || !file.isNull(2);
            } else {
                for (const auto& root : removed)
                    if (binding.path.starts_with(root)) binding.detached = unavailable = true;
            }
            queueChanged_ = policy_.setUnavailable(id, unavailable) || queueChanged_;
        }
        if (std::any_of(chain_.begin(), chain_.end(), [&](uint64_t id) { return policy_.unavailable(id); })) {
            engine_->discardPreloads();
            commitTransition();
            chain_.clear();
            attempted_ = {};
        }
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

    // Library sources arrive as lowercase index keys; the file opens and shows under its on-disk spelling, which is
    // resolved once, when the entry is first opened, since a NAS lookup per queued track would stall large queues.
    const TrackSource& playable(uint64_t id) {
        auto& queued = *std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == id; });
        const auto binding = libraryEntries_.find(id);
        if (binding != libraryEntries_.end() && binding->second.file && !binding->second.spelled) {
            binding->second.spelled = true;
            queued.source = TrackSource(libraryDisplayPath(queued.source.pathUtf8()), queued.source.range(), queued.source.decoder());
        }
        return queued.source;
    }

    // Ends of the previous entry that are still pending belong to playback this call supersedes.
    void startEntry(const QueuePosition& position, bool play) {
        if (policy_.unavailable(position.entryId)) throw std::invalid_argument("queue entry is unavailable");
        engine_->waitIdle();
        const auto before = engine_->snapshot();
        seenTrackEnds_ = before.trackEnds;
        seenTransitions_ = before.transitions;
        chain_.clear();
        const auto& source = playable(position.entryId);
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
            if (!position || position->entryId != id) position = policy_.select(id, true);
            const auto& source = entry(position->entryId).source;
            state_.track = source;
            state_.queueEntryId = position->entryId;
            settings_.lastFile = toWide(source.pathUtf8());
            queueChanged_ = true;
        }
        persist();
    }

    // mpg123 scans the whole stream when playback opens it, so its length replaces a scan-time estimate.
    void recordPlaybackLength() {
        if (!library_ || !state_.queueEntryId) return;
        const auto binding = libraryEntries_.find(state_.queueEntryId);
        if (binding == libraryEntries_.end() || !binding->second.file || binding->second.lengthRecorded) return;
        const auto playback = engine_->snapshot();
        if (!playback.source || !playback.source->totalFrames || playback.decoder != audio::DecoderKind::Mpg123 ||
            playback.file != toWide(entry(state_.queueEntryId).source.pathUtf8()))
            return;
        binding->second.lengthRecorded = true;
        LibraryScanner(*library_).recordExactLength(binding->second.file, playback.source->sampleRate,
                                                    playback.source->totalFrames);
    }

    // A play session lasts while one library entry stays current (docs/Plan.md §7.8).
    void updatePlaySession() {
        if (!library_) return;
        const auto playback = engine_->snapshot();
        const auto now = std::chrono::steady_clock::now();
        // Audio the device can have played since the previous pass; larger jumps are seeks or restarts.
        const auto reachable = [&](uint32_t rate) {
            return uint64_t((std::chrono::duration<double>(now - sessionSeen_).count() * 1.25 + 0.25) * rate);
        };
        if (session_) {
            const bool current = playback.transitions == sessionTransitions_ && playback.trackEnds == sessionTrackEnds_ &&
                                 state_.queueEntryId == sessionEntry_ && playback.file == sessionFile_;
            if (!current || playback.state == audio::PlayState::Stopped) {
                // A gapless transition or natural end heard the rest of the track; a failure keeps what was observed.
                if (playback.transitions != sessionTransitions_ || (playback.trackEnds != sessionTrackEnds_ && !playback.error))
                    session_->finish();
                session_->persist(*library_);
                session_.reset();
            } else if (playback.state == audio::PlayState::Playing) {
                // Rendering can move into a queued file before the engine publishes the transition; a jump back
                // to its start belongs to the next file, so this session waits for the transition instead.
                const auto last = session_->position();
                const bool nextFile = playback.queuedFiles > 0 && last && playback.position < *last &&
                                      playback.position <= reachable(sessionRate_);
                if (!nextFile) session_->observe(playback.position, reachable(sessionRate_));
            } else {
                session_->interrupt();
            }
        }
        if (!session_ && state_.queueEntryId && playback.state != audio::PlayState::Stopped && playback.source &&
            playback.source->sampleRate) {
            const auto binding = libraryEntries_.find(state_.queueEntryId);
            if (binding != libraryEntries_.end() && binding->second.file &&
                playback.file == toWide(entry(state_.queueEntryId).source.pathUtf8())) {
                session_.emplace(LibraryTrackKey{binding->second.file, 0}, playback.source->sampleRate, playback.total);
                sessionEntry_ = state_.queueEntryId;
                sessionFile_ = playback.file;
                sessionTransitions_ = playback.transitions;
                sessionTrackEnds_ = playback.trackEnds;
                sessionRate_ = playback.source->sampleRate;
                sessionPersisted_ = now;
                sessionCountPersisted_ = false;
                // A track that started since the previous pass is credited from its first frame.
                if (playback.state == audio::PlayState::Playing) {
                    session_->observe(0, 0);
                    session_->observe(playback.position, reachable(sessionRate_));
                }
            }
        }
        sessionSeen_ = now;
        if (!session_) return;
        if (now - sessionPersisted_ >= std::chrono::seconds(30) || (session_->counted() && !sessionCountPersisted_)) {
            session_->persist(*library_);
            sessionPersisted_ = now;
            sessionCountPersisted_ = session_->counted();
        }
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
        const auto& source = playable(upcoming->entryId);
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
        state_.settings = settingsDto(settings_);
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
                    updatePlaySession();
                    refreshLibraryAvailability();
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
                refreshLibraryAvailability();
                advanceAfterTrackEnd();
                ensureNextQueued();
                recordPlaybackLength();
                updatePlaySession();
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
        if (session_) session_->persist(*library_);
        engine_.reset();
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    Settings settings_;
    std::filesystem::path settingsFile_;
    std::unique_ptr<audio::PlaybackEngine> engine_;
    std::unique_ptr<ArtworkCache> artwork_;
    std::unique_ptr<LibraryStore> library_;
    std::unique_ptr<LibraryTextIndex> libraryText_;
    std::unique_ptr<LibraryMonitor> libraryMonitor_;
    std::optional<sqlite::Database> libraryReader_;
    int64_t libraryVersion_ = 0;
    struct LibraryEntry {
        std::wstring path;
        int64_t file = 0;
        bool detached = false;
        bool lengthRecorded = false;
        bool spelled = false;
    };
    std::map<uint64_t, LibraryEntry> libraryEntries_;
    std::optional<PlaySession> session_;
    uint64_t sessionEntry_ = 0, sessionTransitions_ = 0, sessionTrackEnds_ = 0;
    std::wstring sessionFile_;
    uint32_t sessionRate_ = 0;
    std::chrono::steady_clock::time_point sessionSeen_ = std::chrono::steady_clock::now(), sessionPersisted_;
    bool sessionCountPersisted_ = false;
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
LibraryTrackPage LocalServer::browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return impl_->browseLibrary(offset, limit, revision, token);
}
LibraryTrackPage LocalServer::searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return impl_->searchTracks(std::move(query), offset, limit, revision, token);
}
LibrarySearchResults LocalServer::searchLibrary(std::string query, int64_t offset, int64_t limit,
                                                std::optional<uint64_t> revision,
                                                const CancellationToken& token) const {
    return impl_->searchLibrary(std::move(query), offset, limit, revision, token);
}
PlayStatistics LocalServer::playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token) const {
    return impl_->playStatistics(query, token);
}
LibraryAlbumPage LocalServer::recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token) const {
    return camel::server::recentlyPlayedAlbums(impl_->library(), offset, limit, token);
}

LibraryAlbumPage LocalServer::browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                           int64_t limit, std::optional<uint64_t> revision, const CancellationToken& token) const {
    return camel::server::browseAlbums(impl_->library(), sort, filter, offset, limit, revision, token);
}
LibraryGroupPage LocalServer::browseGroups(LibraryGroupField field, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return camel::server::browseGroups(impl_->library(), field, offset, limit, revision, token);
}
LibraryTrackRowPage LocalServer::browseTrackRows(TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                                 std::optional<uint64_t> revision, const CancellationToken& token) const {
    return camel::server::browseTrackRows(impl_->library(), sort, descending, offset, limit, revision, token);
}
std::optional<ArtworkReference> LocalServer::artwork(const ArtworkRequest& request, const CancellationToken& token) const {
    return impl_->artwork(request, token);
}

LibraryAlbumDetail LocalServer::albumDetail(int64_t album, const CancellationToken& token) const {
    return camel::server::albumDetail(impl_->library(), album, token);
}
LibraryFolderListing LocalServer::browseFolder(std::optional<int64_t> folder, const CancellationToken& token) const {
    return camel::server::browseFolder(impl_->library(), folder, token);
}
std::vector<LibraryRootInfo> LocalServer::libraryRoots() const {
    return camel::server::libraryRoots(impl_->library());
}
LibraryScanProgress LocalServer::libraryScanProgress() const {
    return impl_->library().scanProgress();
}
std::vector<PlaylistSummary> LocalServer::playlists() const {
    return listPlaylists(impl_->library());
}
PlaylistPage LocalServer::playlistItems(int64_t playlist, int64_t offset, int64_t limit) const {
    return camel::server::playlistItems(impl_->library(), playlist, offset, limit);
}
SelectionPreview LocalServer::previewSelection(const std::vector<PlaylistSource>& sources, const CancellationToken& token) const {
    return camel::server::previewSelection(impl_->library(), sources, token);
}
PlaylistChange LocalServer::createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                           const CancellationToken& token) {
    return camel::server::createPlaylist(impl_->library(), std::move(name), sources, token);
}
PlaylistChange LocalServer::addToPlaylist(int64_t playlist, std::optional<int64_t> revision,
                                          const std::vector<PlaylistSource>& sources, const CancellationToken& token) {
    return camel::server::addToPlaylist(impl_->library(), playlist, revision, sources, token);
}
PlaylistChange LocalServer::saveQueueAsPlaylist(std::string name) {
    return impl_->saveQueueAsPlaylist(std::move(name));
}
PlaylistChange LocalServer::editPlaylist(const PlaylistEdit& edit) {
    return camel::server::editPlaylist(impl_->library(), edit);
}
bool LocalServer::undoPlaylistChange(const PlaylistUndo& undo) {
    return camel::server::undoPlaylistChange(impl_->library(), undo);
}

std::shared_ptr<IServer> createLocalServer() {
    const auto path = settingsPath();
    return std::make_shared<LocalServer>(loadSettings(path), path);
}

} // namespace camel::server
