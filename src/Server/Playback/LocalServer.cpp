// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/LocalServer.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <map>
#include <thread>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"
#include "OutputBridge/DeviceWatcher.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryText.h"
#include "Server/Library/PlayStatistics.h"
#include "Server/Library/Playlists.h"
#include "Server/Playback/QueueSession.h"
#include "Server/Support/DiagnosticBundle.h"

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
    if (c.apiVersion != kApiVersion || c.kind > CommandKind::SetAlbumFavorite)
        throw std::invalid_argument("unsupported Server command/version");
    const bool queueReplace = c.kind == CommandKind::QueueReplace;
    const bool queueSources = queueReplace || c.kind == CommandKind::QueueAdd || c.kind == CommandKind::QueuePlayNext;
    const bool queueIds = c.kind == CommandKind::QueueReorder || c.kind == CommandKind::QueueRemove;
    const bool trackKeys = c.kind == CommandKind::ResetPlayStatistics || c.kind == CommandKind::SetFavorite;
    if (c.source.has_value() != replaces(c.kind) || c.settings.has_value() != (c.kind == CommandKind::ApplySettings) ||
        (c.frame != 0 && c.kind != CommandKind::Seek) || c.sources.empty() == queueSources ||
        (queueReplace ? c.index >= c.sources.size() : c.index != 0) || c.entryIds.empty() == queueIds ||
        (c.entryId != 0) != (c.kind == CommandKind::QueueSelect) ||
        c.shuffle.has_value() != (c.kind == CommandKind::SetShuffle) ||
        c.repeat.has_value() != (c.kind == CommandKind::SetRepeat) || (c.repeat && *c.repeat > RepeatMode::All) ||
        (c.kind == CommandKind::RemoveLibraryRoot ? c.rootId <= 0 : c.rootId != 0) || c.tracks.empty() == trackKeys ||
        c.path.empty() == (c.kind == CommandKind::AddLibraryRoot) ||
        c.albums.empty() == (c.kind == CommandKind::SetAlbumFavorite) ||
        (c.favorite && c.kind != CommandKind::SetFavorite && c.kind != CommandKind::SetAlbumFavorite))
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
        : settings_(settings), settingsFile_(std::move(settingsFile)), factory_(std::move(factory)),
          engine_(std::make_unique<audio::PlaybackEngine>(settings, factory_)) {
        published_.settings = settingsDto(settings_);
        published_.playback = playback(engine_->snapshot());
        const auto directory =
            settingsFile_.empty() ? std::filesystem::path{} : std::filesystem::absolute(settingsFile_).parent_path();
        if (!directory.empty() && std::filesystem::is_directory(directory)) {
            artwork_ = std::make_unique<ArtworkCache>(directory / "thumbs");
            library_ = std::make_unique<LibraryStore>(directory);
            library_->write([](sqlite::Database& db) { updateLibraryNameRules(db); });
            libraryText_ = std::make_unique<LibraryTextIndex>(*library_);
            LibraryMonitor::Options options;
            // Covers being shown take the storage first: scans drop to one worker and fingerprints wait meanwhile.
            options.playbackActive = [this] {
                return std::chrono::steady_clock::now().time_since_epoch().count() <
                       artworkUntil_.load(std::memory_order_relaxed);
            };
            options.playingSource = [this] {
                const auto snapshot = engine_->snapshot();
                return snapshot.state == audio::PlayState::Playing ? std::filesystem::path(snapshot.file)
                                                                   : std::filesystem::path{};
            };
            libraryMonitor_ = std::make_unique<LibraryMonitor>(*library_, std::move(options));
            // Album-wall thumbnails are made ahead of display, only while nothing plays, scans or shows covers.
            coverWarmer_ = std::jthread([this](std::stop_token stop) {
                const auto busy = [this, &stop] {
                    return stop.stop_requested() || library_->scanProgress().scans > 0 ||
                           engine_->snapshot().state == audio::PlayState::Playing ||
                           std::chrono::steady_clock::now().time_since_epoch().count() <
                               artworkUntil_.load(std::memory_order_relaxed);
                };
                while (!stop.stop_requested()) {
                    for (int i = 0; i < 300 && !stop.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (busy()) continue;
                    library_->backupIfDue();
                    try {
                        if (const auto made = artwork_->prepare(*library_, busy))
                            log::info("artwork: prepared %zu album covers", made);
                    } catch (const std::exception& e) {
                        log::warn("artwork: %s", e.what());
                    }
                }
            });
        }
        if (library_ && [&] {
                auto reader = library_->openReader();
                return hasSavedSession(reader);
            }()) {
            std::lock_guard lock(mutex_);
            CommandStamp next = issued_;
            ++next.id;
            ++next.queueRevision;
            queue_.push_back({Command{}, next, true});
            issued_ = next;
        } else if (!settings_.lastFile.empty()) {
            sessionRestored_ = true;
            Command restore;
            restore.kind = CommandKind::Load;
            restore.source.emplace(toUtf8(settings_.lastFile));
            enqueue(std::move(restore), true);
        } else {
            sessionRestored_ = true;
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() { shutdown(); }

    // The cover warmer stops before the worker releases the engine it reads; the worker saves the queue on its way out.
    void shutdown() {
        libraryMonitor_.reset();
        coverWarmer_ = {};
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            listener_ = {};
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (library_) {
            library_->backupIfDue(); // for a day that never had an idle moment
            library_->close();
        }
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

    void writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths, const std::string& appVersion) const {
        DiagnosticInput input;
        input.dataDirectory = std::filesystem::absolute(settingsFile_).parent_path();
        std::wstring executable(32768, wchar_t{});
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
        input.programDirectory = std::filesystem::path(executable).parent_path();
        input.includePaths = includePaths;
        input.appVersion = appVersion;
        input.snapshot = snapshot();
        if (library_) input.roots = server::libraryRoots(*library_);
        if (input.snapshot.playback.state != PlaybackState::Stopped) {
            input.capabilities = "Format support was not probed: the device was in use.\n";
        } else {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                const Settings settings = loadSettings(settingsFile_);
                input.capabilities = probeCapabilities(*factory_(settings), settings);
            } catch (const std::exception& e) {
                input.capabilities = std::string("Format support could not be probed: ") + e.what() + "\n";
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
        }
        server::writeDiagnosticBundle(file, input);
    }

    DataRecovery dataRecovery() const {
        DataRecovery recovery;
        recovery.settings = settingsRecovery_;
        if (library_ && library_->report().damaged)
            recovery.library =
                library_->report().restoredBackup ? DataRecovery::Library::RestoredBackup : DataRecovery::Library::Rebuilt;
        return recovery;
    }
    SettingsRecovery settingsRecovery_ = SettingsRecovery::None;

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

    LibraryTrackPage searchTracks(const std::string& query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token) const {
        if (!library_) throw std::runtime_error("library is not configured");
        return camel::server::searchTracks(*library_, query, offset, limit, revision, token);
    }

    LibrarySearchResults searchLibrary(const std::string& query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                       const CancellationToken& token, bool totals) const {
        if (!library_) throw std::runtime_error("library is not configured");
        return camel::server::searchLibrary(*library_, query, offset, limit, revision, token, totals);
    }

    PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token) const {
        return camel::server::playStatistics(library(), query, token);
    }

    void rereadLibrary() {
        requestMetadataReread(library());
        if (libraryMonitor_) libraryMonitor_->rescanAll();
    }

    void clearArtworkCache() {
        if (artwork_) artwork_->clear();
    }

    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token) const {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        artworkUntil_.store(until.time_since_epoch().count(), std::memory_order_relaxed);
        return artwork_ ? artwork_->get(library_.get(), request, token) : std::nullopt;
    }

    LibraryStore& library() const {
        if (!library_) throw std::runtime_error("library is not configured");
        return *library_;
    }

    // Reading every credit takes a moment on a large library, so the names are kept until the library changes.
    std::shared_ptr<const LibraryCreditNames> creditNames(const CancellationToken& token) const {
        std::lock_guard lock(creditMutex_);
        creditNames_ = libraryCreditNames(library(), creditNames_, token);
        return creditNames_;
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
                if (file.bind(1, key).step()) item.track = LibraryTrackKey{file.integer(0)};
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
        if (entries_.empty() && autoplayAllowed() && appendAutoplay()) {
            startEntry(policy_.select(entries_.front().id), true);
            return;
        }
        const auto current = policy_.current();
        if (engine_->snapshot().state == audio::PlayState::Stopped && !entries_.empty() &&
            (!current || policy_.unavailable(current->entryId))) {
            const auto next = policy_.next(false);
            if (!next) throw std::invalid_argument("queue entries are unavailable");
            startEntry(*next, true);
        } else
            engine_->play();
    }

    // The restored current entry opens its file only now, at the position it was left at.
    void resumeRestored(uint64_t position) {
        const auto current = policy_.current();
        restored_.reset();
        if (!current || policy_.unavailable(current->entryId)) {
            resumePlayback();
            return;
        }
        startEntry(*current, false);
        if (position) engine_->seek(position);
        engine_->play();
    }

    // Library entries are found by their file, wherever it moved, so restoring reads only the library database
    // (docs/Plan.md §7); an unavailable file stays in the queue, marked unavailable.
    void restoreQueueSession() {
        sessionRestored_ = true;
        const auto started = measure::now();
        auto measured = wil::scope_exit([&] {
            if (measure::measuring())
                log::info("measure: queue of %zu entries restored in %.0f ms", entries_.size(),
                          measure::milliseconds(started, measure::now()));
        });
        auto reader = library_->openReader();
        const auto saved = loadSession(reader);
        if (!saved) return;
        std::vector<QueueEntry> entries;
        entries.reserve(saved->entries.size());
        for (const auto& item : saved->entries) {
            entries.push_back({++lastEntryId_, item.source});
            // The library no longer describes a file that is gone, so the entry keeps what it showed.
            entries.back().title = item.description;
            entries.back().automatic = item.automatic;
        }
        // One query for the whole queue: a lookup per entry took 3.4 s for 100k entries.
        auto files = reader.prepare("SELECT q.position,q.file_id,f.path,coalesce(t.title,''),coalesce(t.artist,''),"
                                    "coalesce(nullif(a.title,''),nullif(t.album,''),''),coalesce(a.id,0),t.id IS NOT NULL FROM "
                                    "queue q JOIN files f ON f.id=q.file_id "
                                    "LEFT JOIN tracks t ON t.file_id=f.id "
                                    "LEFT JOIN albums a ON a.id=t.album_id");
        while (files.step()) {
            const auto at = size_t(files.integer(0));
            if (at >= entries.size() || saved->entries[at].file != files.integer(1)) continue;
            auto& entry = entries[at];
            const auto& item = saved->entries[at];
            const auto key = toWide(files.text(2));
            // The saved on-disk spelling is kept while it still names this file; a moved file plays from its new key.
            std::wstring keyText = key;
            if (_wcsnicmp(keyText.c_str(), LR"(\\?\UNC\)", 8) == 0)
                keyText = LR"(\\)" + keyText.substr(8);
            else if (keyText.starts_with(LR"(\\?\)"))
                keyText = keyText.substr(4);
            const auto savedPath = toWide(item.source.pathUtf8());
            const bool spelled = CompareStringOrdinal(savedPath.c_str(), int(savedPath.size()), keyText.c_str(),
                                                      int(keyText.size()), TRUE) == CSTR_EQUAL;
            if (!spelled) entry.source = TrackSource(files.text(2), item.source.range(), item.source.decoder());
            if (files.integer(7)) entry.title = files.text(3); // an untitled track shows its file name, as before the restart
            entry.artist = files.text(4);
            entry.album = files.text(5);
            entry.albumId = files.integer(6);
            libraryEntries_[entry.id] = {key, item.file, false, false, spelled};
        }
        if (!libraryReader_) libraryReader_.emplace(library_->openReader());
        QueuePolicyState state;
        state.entries.reserve(entries.size());
        for (const auto& entry : entries) state.entries.push_back(entry.id);
        for (const auto at : saved->order) state.order.push_back(entries[at].id);
        for (const auto at : saved->history) state.history.push_back(entries[at].id);
        if (saved->current) state.current = entries[*saved->current].id;
        state.shuffle = saved->shuffle;
        state.repeat = saved->repeat;
        policy_.restore(std::move(state));
        applyMarks(entries);
        entries_ = std::move(entries);
        if (const auto current = policy_.current()) {
            state_.queueEntryId = current->entryId;
            state_.track = entries_[current->index].source;
            restored_ = saved->track;
        }
        libraryVersion_ = 0;
        refreshLibraryAvailability();
        queueChanged_ = true;
        auto policy = policy_.state();
        saved_ = {std::move(policy.entries),
                  std::move(policy.order),
                  std::move(policy.history),
                  policy.current,
                  policy.shuffle,
                  policy.repeat,
                  restored_.value_or(SavedTrack{}),
                  engine_->snapshot().state,
                  std::chrono::steady_clock::now()};
    }

    SavedTrack currentTrack(const audio::EngineSnapshot& playback) const {
        if (restored_) return *restored_;
        if (!state_.queueEntryId || !playback.source) return {};
        SavedTrack track{playback.position, playback.total, playback.source, playback.format};
        track.source->md5.reset();
        return track;
    }

    // Written when the queue, its order or the current entry changes, on pause and stop, every 5 seconds while playing
    // (what a crash can lose) and at exit. Large queues are rewritten only when their entries or order change.
    void saveQueueSession(bool final) {
        if (!library_ || !sessionRestored_) return;
        if (sessionWrite_.valid()) {
            if (!final && sessionWrite_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try {
                sessionWrite_.get();
            } catch (const std::exception& e) {
                log::warn("session: %s", e.what());
            }
        }
        const auto playback = engine_->snapshot();
        const auto now = std::chrono::steady_clock::now();
        auto policy = policy_.state();
        const SessionParts parts{policy.entries != saved_.entries, policy.order != saved_.order};
        const auto track = currentTrack(playback);
        const bool changed =
            parts.entries || parts.order || policy.current != saved_.current || policy.history != saved_.history ||
            policy.shuffle != saved_.shuffle || policy.repeat != saved_.repeat || playback.state != saved_.playState ||
            (playback.state == audio::PlayState::Playing ? now - saved_.at >= std::chrono::seconds(5) : track != saved_.track);
        if (!changed && !final) return;
        std::unordered_map<uint64_t, size_t> index;
        index.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i) index.emplace(entries_[i].id, i);
        const auto indexes = [&](const std::vector<uint64_t>& ids) {
            std::vector<size_t> result;
            for (const auto id : ids)
                if (const auto at = index.find(id); at != index.end()) result.push_back(at->second);
            return result;
        };
        SavedSession session;
        if (parts.entries)
            for (const auto& entry : entries_) {
                const auto binding = libraryEntries_.find(entry.id);
                const auto title = entry.title.empty()
                                       ? toUtf8(std::filesystem::path(toWide(entry.source.pathUtf8())).stem().wstring())
                                       : entry.title;
                session.entries.push_back({entry.source, binding == libraryEntries_.end() ? 0 : binding->second.file,
                                           entry.artist.empty() ? title : title + " · " + entry.artist, entry.automatic});
            }
        if (parts.order) session.order = indexes(policy.order);
        session.history = indexes(policy.history);
        if (policy.current)
            if (const auto at = index.find(*policy.current); at != index.end()) session.current = at->second;
        session.shuffle = policy.shuffle;
        session.repeat = policy.repeat;
        session.track = track;
        auto write =
            library_->write([session = std::move(session), parts](sqlite::Database& db) { saveSession(db, session, parts); });
        saved_ = {std::move(policy.entries),
                  std::move(policy.order),
                  std::move(policy.history),
                  policy.current,
                  policy.shuffle,
                  policy.repeat,
                  track,
                  playback.state,
                  now};
        if (!final) {
            sessionWrite_ = std::move(write);
            return;
        }
        try {
            write.get();
        } catch (const std::exception& e) {
            log::warn("session: %s", e.what());
        }
    }

    void execute(const Pending& pending) {
        const auto& c = pending.command;
        state_.command = pending.stamp;
        attempted_ = {}; // a command may restart the stream, so the upcoming entry may be queued again
        checked_ = {};
        state_.commandError.reset();
        state_.controlPanelOpened.reset();
        if (pending.restore && !c.source) {
            restoreQueueSession();
            return;
        }
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
                else if (const auto picked = autoplayAllowed() && appendAutoplay() ? policy_.next(false) : std::nullopt)
                    startEntry(*picked, true);
                else
                    engine_->stop();
                break;
            case CommandKind::Previous: {
                const auto playback = engine_->snapshot();
                const auto previous =
                    restored_ ? policy_.previous(restored_->position, restored_->source ? restored_->source->sampleRate : 0)
                              : policy_.previous(playback.position, playback.source ? playback.source->sampleRate : 0);
                if (!previous) break;
                if (previous->entryId == state_.queueEntryId && restored_)
                    restored_->position = 0;
                else if (previous->entryId == state_.queueEntryId)
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
                if (restored_)
                    resumeRestored(restored_->position);
                else
                    resumePlayback();
                break;
            case CommandKind::Pause:
                engine_->pause();
                break;
            case CommandKind::TogglePause:
                if (restored_)
                    resumeRestored(restored_->position);
                else if (engine_->snapshot().state == audio::PlayState::Stopped)
                    resumePlayback();
                else
                    engine_->togglePause();
                break;
            case CommandKind::Stop:
                engine_->stop();
                if (restored_) restored_->position = 0;
                break;
            case CommandKind::Seek:
                if (restored_) {
                    restored_->position = restored_->total ? std::min(c.frame, restored_->total) : c.frame;
                    break;
                }
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
            case CommandKind::SetFavorite:
            case CommandKind::SetAlbumFavorite:
                if (!library_) throw std::logic_error("Server library is not configured");
                if (c.kind == CommandKind::SetFavorite)
                    setTrackFavorite(*library_, c.tracks, c.favorite);
                else
                    setAlbumFavorite(*library_, c.albums, c.favorite);
                applyMarks(entries_);
                ++marks_;
                queueChanged_ = true;
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
            case CommandKind::RefreshDevices:
                refreshDevices();
                break;
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
            restored_.reset();
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
                        "LEFT JOIN tracks t ON t.file_id=f.id LEFT JOIN albums a ON a.id=t.album_id "
                        "WHERE f.path=? AND f.available=1");
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
        applyMarks(entries);
        return entries;
    }

    // The entries' library files and their favorite marks.
    void applyMarks(std::vector<QueueEntry>& entries) {
        if (!library_) return;
        std::unordered_set<int64_t> favorites;
        try {
            if (!libraryReader_) libraryReader_.emplace(library_->openReader());
            auto rows = libraryReader_->prepare("SELECT file_id FROM stats WHERE favorite<>0");
            while (rows.step()) favorites.insert(rows.integer(0));
        } catch (const std::exception& e) {
            log::warn("queue marks: %s", e.what());
            return;
        }
        for (auto& entry : entries) {
            const auto binding = libraryEntries_.find(entry.id);
            entry.file = binding == libraryEntries_.end() ? 0 : binding->second.file;
            entry.favorite = entry.file && favorites.contains(entry.file);
        }
    }

    void appendQueue(const std::vector<TrackSource>& sources, bool playNext) {
        // The user's tracks play next, so automatic entries that have not started yet go (docs/Plan.md §8.1).
        const auto playing =
            std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == state_.queueEntryId; });
        std::vector<uint64_t> pending;
        for (auto it = playing == entries_.end() ? entries_.begin() : playing + 1; it != entries_.end(); ++it)
            if (it->automatic) pending.push_back(it->id);
        if (!pending.empty()) removeQueue(pending);
        auto entries = buildQueueEntries(sources);
        std::vector<uint64_t> ids;
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.push_back(e.id);
        policy_.append(ids, playNext);
        auto at = entries_.end();
        if (playNext)
            if (const auto current = std::find_if(entries_.begin(), entries_.end(),
                                                  [&](const QueueEntry& e) { return e.id == state_.queueEntryId; });
                current != entries_.end())
                at = current + 1;
        entries_.insert(at, std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
        libraryVersion_ = 0;
        refreshLibraryAvailability();
    }

    void removeQueue(const std::vector<uint64_t>& ids) {
        policy_.remove(ids);
        if (restored_ && std::find(ids.begin(), ids.end(), state_.queueEntryId) != ids.end()) {
            restored_.reset();
            state_.queueEntryId = 0;
            state_.track.reset();
        }
        std::erase_if(entries_, [&](const QueueEntry& e) { return std::find(ids.begin(), ids.end(), e.id) != ids.end(); });
        for (const auto id : ids) libraryEntries_.erase(id);
    }

    void clearQueue() {
        const uint64_t current = state_.queueEntryId;
        const auto keep = std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == current; });
        policy_.clearExcept(keep == entries_.end() ? std::nullopt : std::optional(current));
        if (keep == entries_.end())
            entries_.clear();
        else {
            QueueEntry currentEntry = *keep;
            entries_ = {std::move(currentEntry)};
        }
        std::erase_if(libraryEntries_, [&](const auto& item) { return item.first != current; });
    }

    // `idle` passes may skip a large queue for up to a second, since a scan changes the library many times a second.
    void refreshLibraryAvailability(bool idle = false) {
        if (!libraryReader_ || libraryEntries_.empty()) return;
        const auto version = libraryReader_->scalar("PRAGMA data_version").value();
        if (version == libraryVersion_) return;
        const bool large = libraryEntries_.size() > 2000;
        const auto now = std::chrono::steady_clock::now();
        if (idle && large && now - availabilityChecked_ < std::chrono::seconds(1)) return;
        availabilityChecked_ = now;
        libraryVersion_ = version;
        std::vector<std::wstring> removed;
        auto roots = libraryReader_->prepare("SELECT path FROM roots WHERE removed IS NOT NULL");
        while (roots.step()) {
            auto path = toWide(roots.text(0));
            if (!path.ends_with(L'\\')) path.push_back(L'\\');
            removed.push_back(std::move(path));
        }
        // A large queue reads the unavailable files once instead of asking about each entry, which kept the worker busy
        // for seconds on every change of a 100k-entry queue.
        std::unordered_set<int64_t> gone;
        if (large) {
            auto files = libraryReader_->prepare("SELECT f.id FROM files f JOIN roots r ON r.id=f.root_id "
                                                 "WHERE f.available=0 OR r.online=0 OR r.removed IS NOT NULL");
            while (files.step()) gone.insert(files.integer(0));
        }
        auto file = libraryReader_->prepare("SELECT f.available,r.online,r.removed FROM files f "
                                            "JOIN roots r ON r.id=f.root_id WHERE f.id=?");
        for (auto& [id, binding] : libraryEntries_) {
            bool unavailable = binding.detached;
            if (binding.file && large) {
                unavailable = gone.contains(binding.file);
            } else if (binding.file) {
                file.reset();
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
            checked_ = {};
        }
    }

    void reorderQueue(const std::vector<uint64_t>& ids) {
        policy_.reorder(ids);
        std::unordered_map<uint64_t, QueueEntry*> byId;
        byId.reserve(entries_.size());
        for (auto& entry : entries_) byId.emplace(entry.id, &entry);
        std::vector<QueueEntry> entries;
        entries.reserve(ids.size());
        for (const auto id : ids) entries.push_back(std::move(*byId.at(id)));
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
            queued.source =
                TrackSource(libraryDisplayPath(queued.source.pathUtf8()), queued.source.range(), queued.source.decoder());
            queueChanged_ = true;
        }
        return queued.source;
    }

    // Ends of the previous entry that are still pending belong to playback this call supersedes.
    void startEntry(const QueuePosition& position, bool play) {
        if (policy_.unavailable(position.entryId)) throw std::invalid_argument("queue entry is unavailable");
        restored_.reset();
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
                const bool nextFile =
                    playback.queuedFiles > 0 && last && playback.position < *last && playback.position <= reachable(sessionRate_);
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
                session_.emplace(LibraryTrackKey{binding->second.file}, playback.source->sampleRate, playback.total);
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
        // Predicting copies the policy, which is costly for a large queue; it is redone only when its inputs changed.
        const QueueCheck check{policy_.revision(), state_.queueEntryId, {chain_.begin(), chain_.end()}, playback.nextSlotFree};
        if (check == checked_) return;
        checked_ = check;
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
        if (const auto next = policy_.next(true))
            startEntry(*next, true);
        else if (const auto picked = autoplayAllowed() && appendAutoplay() ? policy_.next(true) : std::nullopt)
            startEntry(*picked, true);
    }

    // Autoplay (docs/Plan.md §8.1): once the last entry plays, a random library track follows it, so it preloads
    // for gapless playback. Each entry gets one try; a repeat mode never ends the queue.
    bool autoplayAllowed() const { return settings_.autoplay && library_ && policy_.repeat() == RepeatMode::Off; }

    void extendWithAutoplay() {
        if (!autoplayAllowed() || !state_.queueEntryId || autoplayTried_ == state_.queueEntryId) return;
        if (engine_->snapshot().state != audio::PlayState::Playing) return;
        autoplayTried_ = state_.queueEntryId;
        if (policy_.upcoming()) return;
        try {
            appendAutoplay();
        } catch (const std::exception& e) {
            log::warn("autoplay: %s", e.what());
        }
    }

    // A random available library track not played in the last 7 days and not in the queue; when every track was
    // played that recently, the one played longest ago.
    std::optional<TrackSource> pickAutoplayTrack() {
        if (!libraryReader_) libraryReader_.emplace(library_->openReader());
        nlohmann::json queued = nlohmann::json::array();
        for (const auto& [id, binding] : libraryEntries_)
            if (binding.file) queued.push_back(binding.file);
        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        const std::string from = " FROM tracks t JOIN files f ON f.id=t.file_id JOIN roots r ON r.id=f.root_id "
                                 "WHERE f.available=1 AND r.online=1 AND r.removed IS NULL "
                                 "AND t.file_id NOT IN (SELECT value FROM json_each(?1))";
        auto fresh = libraryReader_->prepare("SELECT f.path" + from +
                                             " AND NOT EXISTS(SELECT 1 FROM play_events e WHERE "
                                             "e.file_id=t.file_id AND e.started>=?2) ORDER BY random() LIMIT 1");
        fresh.bind(1, queued.dump()).bind(2, int64_t(now.count()) - int64_t(7) * 24 * 3600 * 1000);
        if (fresh.step()) return TrackSource(fresh.text(0));
        auto oldest = libraryReader_->prepare(
            "SELECT f.path" + from + " ORDER BY (SELECT MAX(e.started) FROM play_events e WHERE e.file_id=t.file_id) LIMIT 1");
        oldest.bind(1, queued.dump());
        if (oldest.step()) return TrackSource(oldest.text(0));
        return std::nullopt;
    }

    bool appendAutoplay() {
        const auto source = pickAutoplayTrack();
        if (!source) return false;
        auto entries = buildQueueEntries({*source});
        entries.front().automatic = true;
        policy_.append({entries.front().id}, false);
        entries_.push_back(std::move(entries.front()));
        libraryVersion_ = 0;
        refreshLibraryAvailability();
        queueChanged_ = true;
        // Played automatic entries beyond the latest 100 go, oldest first; the user's entries stay.
        std::vector<uint64_t> played;
        for (const auto& entry : entries_) {
            if (entry.id == state_.queueEntryId) break;
            if (entry.automatic) played.push_back(entry.id);
        }
        if (played.size() > kAutoplayKept) {
            played.resize(played.size() - kAutoplayKept);
            removeQueue(played);
        }
        return true;
    }

    QueueSnapshot queueSnapshot() const {
        QueueSnapshot queue;
        queue.revision = policy_.revision();
        queue.marks = marks_;
        queue.entries = entries_;
        if (const auto upcoming = policy_.upcoming()) queue.upcomingEntryId = upcoming->entryId;
        queue.shuffle = policy_.shuffle();
        queue.repeat = policy_.repeat();
        return queue;
    }

    void publish(bool completed) {
        auto p = playback(engine_->snapshot());
        if (restored_ && p.state == PlaybackState::Stopped && state_.track) {
            p.state = PlaybackState::Paused;
            p.pathUtf8 = state_.track->pathUtf8();
            p.position = restored_->position;
            p.total = restored_->total;
            p.source = restored_->source;
            p.format = restored_->format;
        }
        if (!completed && !queueChanged_ && p.revision == state_.playback.revision && p.position == state_.playback.position &&
            p.underruns == state_.playback.underruns && state_.devices.revision == published_.devices.revision)
            return;
        state_.playback = p;
        state_.settings = settingsDto(settings_);
        // Rebuilt only when it changed: a large queue is not copied on every position update.
        if (queueChanged_ || state_.queue.revision != policy_.revision() || publishedEntry_ != state_.queueEntryId) {
            state_.queue = queueSnapshot();
            publishedEntry_ = state_.queueEntryId;
        }
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

    void refreshDevices() {
        DeviceCatalog catalog;
        catalog.wasapi = devices(audio::listWasapiDevices());
        catalog.asio = devices(audio::listAsioDrivers());
        catalog.ks = devices(audio::listKsPins());
        catalog.revision = state_.devices.revision + 1;
        state_.devices = std::move(catalog);
    }

    // A DAC switched on or off raises several endpoint events, and its KS pin and ASIO device can follow later; the
    // lists are read again once the events stop, so a device switched on after the start can be chosen.
    void refreshDevicesAfterChange() {
        using Clock = std::chrono::steady_clock;
        const int64_t changed = devicesChangedAt_.load(std::memory_order_acquire);
        if (!changed || Clock::now() - Clock::time_point(Clock::duration(changed)) < kDevicesSettle) return;
        devicesChangedAt_.store(0, std::memory_order_release);
        refreshDevices();
    }

    void run() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_ptr<audio::DeviceWatcher> deviceWatcher;
        try {
            deviceWatcher = audio::watchAudioDevices([this] {
                devicesChangedAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
            });
        } catch (const audio::Error& e) {
            log::warn("device lists will not follow device changes: %s", e.what());
        }
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
                refreshLibraryAvailability(true);
                advanceAfterTrackEnd();
                extendWithAutoplay();
                ensureNextQueued();
                recordPlaybackLength();
                updatePlaySession();
                refreshDevicesAfterChange();
            } catch (const std::exception& e) {
                log::error("Server queue advance failed: %s", e.what());
            }
            saveQueueSession(false);
            publish(pending.has_value());
            if (pending) {
                {
                    std::lock_guard lock(mutex_);
                    processed_ = pending->stamp.id;
                }
                idle_.notify_all();
            }
        }
        deviceWatcher.reset();
        if (session_) session_->persist(*library_);
        saveQueueSession(true);
        engine_.reset();
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    Settings settings_;
    std::filesystem::path settingsFile_;
    audio::OutputFactory factory_;
    std::unique_ptr<audio::PlaybackEngine> engine_;
    mutable std::atomic<int64_t> artworkUntil_{0}; // steady_clock ticks; read by scan workers, so declared before them
    std::unique_ptr<ArtworkCache> artwork_;
    std::unique_ptr<LibraryStore> library_;
    mutable std::mutex creditMutex_;
    mutable std::shared_ptr<const LibraryCreditNames> creditNames_;
    std::unique_ptr<LibraryTextIndex> libraryText_;
    std::unique_ptr<LibraryMonitor> libraryMonitor_;
    std::jthread coverWarmer_; // uses artwork_, library_ and engine_, so it stops before they go
    std::optional<sqlite::Database> libraryReader_;
    int64_t libraryVersion_ = 0;
    std::chrono::steady_clock::time_point availabilityChecked_;
    struct LibraryEntry {
        std::wstring path;
        int64_t file = 0;
        bool detached = false;
        bool lengthRecorded = false;
        bool spelled = false;
    };
    std::map<uint64_t, LibraryEntry> libraryEntries_;
    uint64_t marks_ = 0;
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
    bool queueChanged_ = false;  // queue state changed without a command or playback change
    uint64_t autoplayTried_ = 0; // the entry whose start autoplay already extended the queue for
    static constexpr size_t kAutoplayKept = 100;
    uint64_t publishedEntry_ = UINT64_MAX; // the current entry state_.queue was built for
    // Gapless: the entry queued in the engine, and the last situation already tried so it is not retried every poll.
    struct QueueAttempt {
        uint64_t current = 0, upcoming = 0, revision = 0;
        size_t depth = 0;
        bool operator==(const QueueAttempt&) const = default;
    };
    uint64_t seenTransitions_ = 0;
    std::deque<uint64_t> chain_; // entries queued in the engine, in playback order
    QueueAttempt attempted_;
    struct QueueCheck {
        uint64_t revision = 0, current = 0;
        std::vector<uint64_t> chain;
        bool slotFree = false;
        bool operator==(const QueueCheck&) const = default;
    };
    QueueCheck checked_;
    // The saved session (M5-R01): the current entry restored but not opened yet, and what was last written.
    std::optional<SavedTrack> restored_;
    bool sessionRestored_ = false; // nothing is saved before the saved session is read back
    struct SavedState {
        std::vector<uint64_t> entries, order, history;
        std::optional<uint64_t> current;
        bool shuffle = false;
        RepeatMode repeat = RepeatMode::Off;
        SavedTrack track;
        audio::PlayState playState = audio::PlayState::Stopped;
        std::chrono::steady_clock::time_point at;
    } saved_;
    std::future<void> sessionWrite_;
    std::function<void(const Event&)> listener_;
    static constexpr std::chrono::seconds kDevicesSettle{1};
    std::atomic<int64_t> devicesChangedAt_{0}; // steady_clock ticks of the last device event; 0 when none is pending
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
void LocalServer::shutdown() {
    impl_->shutdown();
}
LibraryTrackPage LocalServer::browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return impl_->browseLibrary(offset, limit, revision, token);
}
LibraryTrackPage LocalServer::searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return impl_->searchTracks(query, offset, limit, revision, token);
}
LibrarySearchResults LocalServer::searchLibrary(std::string query, int64_t offset, int64_t limit,
                                                std::optional<uint64_t> revision, const CancellationToken& token,
                                                bool totals) const {
    return impl_->searchLibrary(query, offset, limit, revision, token, totals);
}
PlayStatistics LocalServer::playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token) const {
    return impl_->playStatistics(query, token);
}
void LocalServer::rereadLibrary() {
    impl_->rereadLibrary();
}

void LocalServer::clearArtworkCache() {
    impl_->clearArtworkCache();
}

LibraryAlbumPage LocalServer::recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token) const {
    return camel::server::recentlyPlayedAlbums(impl_->library(), offset, limit, token);
}

LibraryAlbumPage LocalServer::browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                           int64_t limit, std::optional<uint64_t> revision,
                                           const CancellationToken& token) const {
    return camel::server::browseAlbums(impl_->library(), sort, filter, offset, limit, revision, token);
}
LibraryGroupPage LocalServer::browseGroups(LibraryGroupField field, int64_t offset, int64_t limit,
                                           std::optional<uint64_t> revision, const CancellationToken& token) const {
    return camel::server::browseGroups(impl_->library(), field, offset, limit, revision, token);
}
std::vector<LibraryInitial> LocalServer::groupInitials(LibraryGroupField field, const CancellationToken& token) const {
    return camel::server::groupInitials(impl_->library(), field, token);
}
std::vector<LibraryInitial> LocalServer::albumInitials(AlbumSort sort, const CancellationToken& token) const {
    return camel::server::albumInitials(impl_->library(), sort, token);
}
std::vector<std::vector<LibraryNamePart>> LocalServer::creditParts(const std::vector<std::string>& credits,
                                                                   const CancellationToken& token) const {
    const auto names = impl_->creditNames(token);
    std::vector<std::vector<LibraryNamePart>> parts;
    parts.reserve(credits.size());
    for (const auto& credit : credits) parts.push_back(camel::server::creditParts(*names, credit));
    return parts;
}
LibraryTrackRowPage LocalServer::browseTrackRows(TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                                 std::optional<uint64_t> revision, const CancellationToken& token) const {
    return camel::server::browseTrackRows(impl_->library(), sort, descending, offset, limit, revision, token);
}
LibraryTrackRowPage LocalServer::smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                             const CancellationToken& token) const {
    return camel::server::smartTracks(impl_->library(), list, offset, limit, revision, token);
}
LibraryAlbumPage LocalServer::favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                             const CancellationToken& token) const {
    return camel::server::favoriteAlbums(impl_->library(), offset, limit, revision, token);
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
PlaylistChange LocalServer::importPlaylist(const std::filesystem::path& file) {
    return importPlaylistFile(impl_->library(), file);
}
void LocalServer::exportPlaylist(int64_t playlist, const std::filesystem::path& file) const {
    exportPlaylistFile(impl_->library(), playlist, file);
}
void LocalServer::writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths,
                                        const std::string& appVersion) const {
    impl_->writeDiagnosticBundle(file, includePaths, appVersion);
}

DataRecovery LocalServer::dataRecovery() const {
    return impl_->dataRecovery();
}
void LocalServer::setSettingsRecovery(SettingsRecovery recovery) {
    impl_->settingsRecovery_ = recovery;
}

std::shared_ptr<IServer> createLocalServer() {
    const auto path = settingsPath();
    auto recovery = SettingsRecovery::None;
    const auto settings = openSettings(path, recovery);
    auto server = std::make_shared<LocalServer>(settings, path);
    server->setSettingsRecovery(recovery);
    return server;
}

} // namespace camel::server
