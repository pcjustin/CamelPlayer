// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/FileDetails.h"
#include "Server/Library/Waveform.h"
#include "Server/Library/Lyrics.h"
#include "Server/Playback/LocalServer.h"
#include <fstream>

#include "Server/Network/AirPlayOutput.h"
#include "Server/Network/Firewall.h"
#include "Server/Network/NetworkOutput.h"
#include "Server/Network/PlayerFinder.h"
#include "Server/Network/PlayerVolume.h"
#include "Server/Playback/Outputs.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <map>
#include <shared_mutex>
#include <thread>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"
#include "OutputBridge/DeviceWatcher.h"
#include "Server/Integration/DiscordPresence.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryText.h"
#include "Server/Library/Loudness.h"
#include "Server/Library/PlayStatistics.h"
#include "Server/Library/Playlists.h"
#include "Server/Playback/QueueSession.h"
#include "Server/Playback/ReplayGain.h"
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

constexpr int kPlayerCoverPixels = 512;

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
    if (c.apiVersion != kApiVersion || c.kind > CommandKind::SetReceiverVolume)
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
        c.playerVolume.has_value() != (c.kind == CommandKind::SetPlayerVolume || c.kind == CommandKind::SetReceiverVolume) ||
        (c.playerVolume && *c.playerVolume > 100) || c.receiver.empty() == (c.kind == CommandKind::SetReceiverVolume) ||
        c.playerMute.has_value() != (c.kind == CommandKind::SetPlayerMute) ||
        (c.kind == CommandKind::RemoveLibraryRoot ? c.rootId <= 0 : c.rootId != 0) || c.tracks.empty() == trackKeys ||
        c.path.empty() == (c.kind == CommandKind::AddLibraryRoot) ||
        c.albums.empty() == (c.kind == CommandKind::SetAlbumFavorite) ||
        c.artists.empty() == (c.kind == CommandKind::SetArtistFavorite) ||
        (c.favorite && c.kind != CommandKind::SetFavorite && c.kind != CommandKind::SetAlbumFavorite &&
         c.kind != CommandKind::SetArtistFavorite))
        throw std::invalid_argument("invalid Server command payload");
    if (std::ranges::any_of(c.artists, [](const LibraryGroupFilter& a) {
            return (a.field != LibraryGroupField::Artist && a.field != LibraryGroupField::Composer) || a.value.empty() ||
                   a.role != LibraryAlbumRole::Any;
        }))
        throw std::invalid_argument("invalid favorite artist");
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
    p.bufferingSeconds = s.bufferingSeconds;
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
    _No_competing_thread_ Impl(const Settings& settings, std::filesystem::path settingsFile, audio::OutputFactory factory)
        : settings_(settings), settingsFile_(std::move(settingsFile)),
          factory_([this, given = std::move(factory)](const Settings& s) {
              return s.backend == Backend::Network   ? networkOutput(s)
                     : s.backend == Backend::AirPlay ? airplayOutput(s)
                                                     : given(s);
          }),
          engine_(std::make_unique<audio::PlaybackEngine>(settings, factory_)) {
        published_.settings = settingsDto(settings_);
        published_.playback = playback(engine_->snapshot());
        const auto directory =
            settingsFile_.empty() ? std::filesystem::path{} : std::filesystem::absolute(settingsFile_).parent_path();
        if (!directory.empty() && std::filesystem::is_directory(directory)) {
            artwork_ = std::make_unique<ArtworkCache>(directory / "thumbs");
            library_ = std::make_unique<LibraryStore>(directory);
            library_->write([](sqlite::Database& db) { updateLibraryNameRules(db); });
            // Folders an earlier run removed, or one that ended before deleting them, go now.
            LibraryScanner(*library_).deleteRemovedRoots();
            libraryText_ = std::make_unique<LibraryTextIndex>(*library_);
            LibraryMonitor::Options options;
            // Covers being shown take the storage first: scans and fingerprints drop to one worker meanwhile.
            options.coversShown = [this] {
                return std::chrono::steady_clock::now().time_since_epoch().count() <
                       artworkUntil_.load(std::memory_order_relaxed);
            };
            options.playback = [this] { return playbackReads(); };
            libraryMonitor_ = std::make_unique<LibraryMonitor>(*library_, std::move(options));
            // Album-wall thumbnails are made ahead of display, only while nothing plays, scans or shows covers.
            coverWarmer_ = std::jthread([this](std::stop_token stop) {
                int64_t swept = -1;
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
                        // The thumbnails of covers whose files left the library go with them.
                        auto reader = library_->openReader();
                        if (const auto deletions = libraryDeletions(reader); deletions != swept) {
                            swept = deletions;
                            if (const auto forgotten = artwork_->forgetDeleted(*library_))
                                log::info("artwork: %zu thumbnails of deleted covers removed", forgotten);
                        }
                        if (const auto made = artwork_->prepare(*library_, busy))
                            log::info("artwork: prepared %zu album covers", made);
                        if (!busy()) library_->caughtUp(BackgroundWork::Covers);
                    } catch (const std::exception& e) {
                        log::warn("artwork: %s", e.what());
                    }
                }
            });
            shareLoudnessInputs();
            LoudnessAnalyzer::Options loudness;
            loudness.settings = [this] {
                std::lock_guard lock(loudnessMutex_);
                return loudnessSettings_;
            };
            loudness.decoders = [this] {
                std::lock_guard lock(loudnessMutex_);
                return loudnessDecoders_;
            };
            loudness.queued = [this] {
                std::lock_guard lock(loudnessMutex_);
                return queuedFiles_;
            };
            loudness.playback = [this] { return playbackReads(); };
            loudness.waveform = [this] {
                std::lock_guard lock(loudnessMutex_);
                return waveformSettings_;
            };
            loudness.playing = [this] {
                std::lock_guard lock(loudnessMutex_);
                return playingTracks_;
            };
            loudness_ = std::make_unique<LoudnessAnalyzer>(*library_, std::move(loudness));
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
        // Discord's cover comes from the file or its folder, as the playback bar's does, at 512 px; cloudflared lives in
        // the data folder, so without one there are no covers.
        discord_ = std::make_unique<DiscordPresence>(
            settingsFile_.empty() ? std::filesystem::path() : settingsFile_.parent_path() / L"tools" / L"cloudflared.exe",
            [](const std::string& path, const CancellationToken& token) -> std::vector<uint8_t> {
                const std::filesystem::path file(toWide(path));
                ArtworkQuery query;
                query.albumFolder = file.parent_path();
                query.firstTrack = file;
                ArtworkChain chain;
                chain.add(makeEmbeddedArtworkProvider());
                chain.add(makeFolderArtworkProvider());
                chain.add(makeParentFolderArtworkProvider());
                const auto image = chain.first(query, token, false);
                if (!image || image->bytes.empty()) return {};
                return squarePicture(image->bytes, 512);
            });
        discord_->configure(settings_.discord);
        worker_ = std::thread([this] { run(); });
    }

    _No_competing_thread_ ~Impl() { shutdown(); }

    // The worker saves the queue first, as a scan at background priority can wait minutes for the CPU on a busy machine.
    // Scans, the cover warmer and the loudness analysis then stop before the worker releases the engine they read.
    void shutdown() {
        if (libraryMonitor_) libraryMonitor_->cancel();
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            listener_ = {};
        }
        cv_.notify_one();
        const bool working = worker_.joinable();
        if (working) queueSaved_.get_future().wait();
        release(loudness_);
        release(libraryMonitor_);
        coverWarmer_ = {};
        if (working) {
            backgroundStopped_.set_value();
            worker_.join();
        }
        release(discord_); // clears the activity
        libraryReader_.reset();
        if (library_) {
            library_->backupIfDue(); // for a day that never had an idle moment
            library_->close();
        }
    }

    // Other threads may be reading the part; it goes once none is, and outside the lock, as stopping it can call back.
    template <typename Part> void release(std::unique_ptr<Part>& part) {
        std::unique_ptr<Part> gone;
        std::unique_lock lock(partsMutex_);
        gone = std::move(part);
        lock.unlock();
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

    DiscordStatus discordStatus() const {
        std::shared_lock parts(partsMutex_);
        return discord_ ? discord_->status() : DiscordStatus{};
    }

    // Read on the caller's thread, which is never the Server's: a file on a NAS can take a moment.
    Lyrics lyrics(const std::string& pathUtf8) const {
        {
            std::scoped_lock lock(lyricsMutex_);
            if (lyricsPath_ == pathUtf8) return lyrics_;
        }
        Lyrics read = readLyrics(std::filesystem::path(toWide(pathUtf8)));
        std::scoped_lock lock(lyricsMutex_);
        lyricsPath_ = pathUtf8;
        lyrics_ = read;
        return read;
    }
    std::optional<std::vector<uint8_t>> waveform(const std::string& pathUtf8) const {
        std::shared_lock parts(partsMutex_);
        if (!library_ || !loudness_) return std::nullopt;
        {
            std::lock_guard lock(loudnessMutex_);
            if (!waveformSettings_.enabled) return std::nullopt;
        }
        const std::filesystem::path given(toWide(pathUtf8));
        auto db = library_->openReader();
        auto file = db.prepare("SELECT id FROM files WHERE path=? AND available=1");
        if (file.bind(1, toUtf8(spelledLibraryPath(given).wstring())).step()) return storedWaveform(db, file.integer(0));
        return loudness_->playingWaveform(given);
    }

    FileDetails fileDetails(const std::string& pathUtf8) const {
        if (!library_) return readFileDetails(pathUtf8, nullptr);
        auto reader = library_->openReader();
        return readFileDetails(pathUtf8, &reader);
    }
    void setDiscordTexts(const DiscordTexts& texts) {
        std::shared_lock parts(partsMutex_);
        if (discord_) discord_->setTexts(texts);
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
        while (processed_ < target) idle_.wait(lock);
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
        std::shared_lock parts(partsMutex_);
        if (libraryMonitor_) libraryMonitor_->rescanAll();
    }

    LibraryScanStart scanLibraryNow(std::optional<int64_t> folder) {
        auto db = library().openReader();
        std::shared_lock parts(partsMutex_);
        if (!libraryMonitor_) throw std::logic_error("Server library is not configured");
        if (library().scanProgress().scans > 0) return LibraryScanStart::Running;
        if (!folder) {
            if (db.scalar("SELECT COUNT(*) FROM roots WHERE removed IS NULL AND online=1").value_or(0) == 0)
                return LibraryScanStart::Offline;
            libraryMonitor_->scanAll();
            return LibraryScanStart::Started;
        }
        auto row = db.prepare("SELECT f.path,f.root_id,r.online FROM folders f JOIN roots r ON r.id=f.root_id "
                              "WHERE f.id=? AND r.removed IS NULL");
        if (!row.bind(1, *folder).step()) throw std::invalid_argument("unknown library folder");
        if (row.integer(2) == 0) return LibraryScanStart::Offline;
        libraryMonitor_->scanFolder(row.integer(1), std::filesystem::path(toWide(row.text(0))));
        return LibraryScanStart::Started;
    }

    void clearArtworkCache() {
        if (artwork_) artwork_->clear();
    }

    void setFinderOptions(const FinderOptions& options) { finderOptions_ = options; }
    void setAirPlayFinderOptions(const AirPlayFinderOptions& options) { airplayFinderOptions_ = options; }

    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token) const {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        artworkUntil_.store(until.time_since_epoch().count(), std::memory_order_relaxed);
        return artwork_ ? artwork_->get(library_.get(), request, token) : std::nullopt;
    }

    LibraryStore& library() const {
        if (!library_) throw std::runtime_error("library is not configured");
        return *library_;
    }

    LoudnessProgress loudnessProgress() const {
        std::shared_lock parts(partsMutex_);
        return loudness_ ? loudness_->progress() : LoudnessProgress{};
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
                saved_ = {}; // nothing of it is in the database: the next save writes it all
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
                libraryMonitor_->scanAdded(LibraryScanner(*library_).addRoot(std::filesystem::path(toWide(c.path))));
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
            case CommandKind::SetPlayerVolume:
                if (playerVolume_)
                    playerVolume_->setLevel(*c.playerVolume);
                else if (receiverVolumeWanted())
                    changeReceiverLevel([&](AirPlayLevel& level) { level.level = *c.playerVolume; });
                break;
            case CommandKind::SetPlayerMute:
                if (playerVolume_)
                    playerVolume_->setMuted(*c.playerMute);
                else if (receiverVolumeWanted())
                    changeReceiverLevel([&](AirPlayLevel& level) { level.muted = *c.playerMute; });
                break;
            case CommandKind::SetReceiverVolume:
                if (receiverVolumeWanted() && c.playerVolume &&
                    std::ranges::find(settings_.airplayGroup, c.receiver) != settings_.airplayGroup.end()) {
                    settings_.airplayLevels.try_emplace(c.receiver).first->second.level = std::min(*c.playerVolume, 100u);
                    groupBaseline_.reset();
                    persist();
                }
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
            case CommandKind::SetArtistFavorite:
                if (!library_) throw std::logic_error("Server library is not configured");
                setArtistFavorite(*library_, c.artists, c.favorite);
                break;
            case CommandKind::ApplySettings: {
                if (c.settings) {
                    const auto lastFile = settings_.lastFile;
                    const auto replayGain = settings_.replayGain;
                    const auto crossfade = settings_.crossfade;
                    const bool adoptOutputSets = settings_.adoptOutputSets;
                    const auto outputs = settings_.outputs;
                    auto airplayLevels = std::move(settings_.airplayLevels);
                    settings_ = nativeSettings(*c.settings);
                    settings_.airplayLevels = std::move(airplayLevels);
                    std::erase_if(settings_.airplayLevels, [&](const auto& entry) {
                        return std::ranges::none_of(settings_.outputs, [&](const SavedOutput& o) {
                            return o.backend == Backend::AirPlay && o.id == entry.first;
                        });
                    });
                    settings_.lastFile = lastFile;
                    settings_.adoptOutputSets = adoptOutputSets;
                    syncOutputs(settings_, state_.devices);
                    checkEnabledReceivers(outputs);
                    followAirPlayGroup();
                    engine_->applySettings(settings_);
                    discord_->configure(settings_.discord);
                    shareLoudnessInputs();
                    // A queued track took the old gain and crossfade; it is queued again with the new ones. The playing
                    // one keeps its.
                    if ((settings_.replayGain != replayGain || settings_.crossfade != crossfade) && !chain_.empty() &&
                        engine_->clearNext()) {
                        chain_.pop_back();
                        attempted_ = {};
                        checked_ = {};
                    }
                    persist();
                }
                break;
            }
            case CommandKind::RefreshDevices:
                refreshDevices();
                break;
            case CommandKind::ShowControlPanel:
                try {
                    state_.controlPanelOpened = engine_->showControlPanel();
                } catch (const audio::Error& e) {
                    state_.controlPanelOpened = false;
                    state_.commandError = Failure{e.code(), e.what()};
                }
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
        // One statement serves every entry: preparing it for each took most of 15 s for a 55,000-track folder.
        std::optional<sqlite::Statement> lookup;
        if (libraryReader_) {
            try {
                lookup.emplace(
                    libraryReader_->prepare("SELECT f.id,coalesce(t.title,''),coalesce(t.artist,''),"
                                            "coalesce(nullif(a.title,''),nullif(t.album,''),''),coalesce(a.id,0) FROM files f "
                                            "LEFT JOIN tracks t ON t.file_id=f.id LEFT JOIN albums a ON a.id=t.album_id "
                                            "WHERE f.path=? AND f.available=1"));
            } catch (const std::exception& e) {
                log::warn("library queue source: %s", e.what());
            }
        }
        for (const auto& source : sources) {
            const auto id = ++lastEntryId_;
            entries.push_back({id, source});
            if (lookup) {
                try {
                    auto& binding = libraryEntries_[id];
                    const std::filesystem::path given(toWide(source.pathUtf8()));
                    auto& file = *lookup;
                    file.reset();
                    // A path the library gave out is found as spelled; resolving every entry on a share took seconds
                    // for an album and held back the play command. Other spellings, such as a mapped drive, resolve.
                    auto key = spelledLibraryPath(given).wstring();
                    bool found = file.bind(1, toUtf8(key)).step();
                    if (!found) {
                        key = canonicalLibraryPath(given).wstring();
                        file.reset();
                        found = file.bind(1, toUtf8(key)).step();
                    }
                    binding.path = key;
                    if (found) {
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

    // The Engine may already decode entries about to leave the queue; it drops them while the queue still knows them,
    // so a transition already heard becomes current first.
    void discardPreloads(const std::function<bool(uint64_t)>& leaving) {
        if (std::ranges::none_of(chain_, leaving)) return;
        engine_->discardPreloads();
        commitTransition();
        chain_.clear();
        attempted_ = {};
        checked_ = {};
    }

    void removeQueue(const std::vector<uint64_t>& ids) {
        discardPreloads([&](uint64_t id) { return std::ranges::find(ids, id) != ids.end(); });
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
        discardPreloads([](uint64_t) { return true; });
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
        auto& reader = *libraryReader_;
        const auto version = reader.scalar("PRAGMA data_version").value();
        if (version == libraryVersion_) return;
        const bool large = libraryEntries_.size() > 2000;
        const auto now = std::chrono::steady_clock::now();
        // Availability comes from files and roots alone, and a large queue reads all of files: other commits, such as
        // the queue session saved every 5 s while playing, are left for the once-a-minute pass.
        const auto fileChanges = library_->fileChanges();
        if (large && libraryVersion_ && fileChanges == availabilityFiles_ && now - availabilityChecked_ < std::chrono::minutes(1))
            return;
        if (idle && large && now - availabilityChecked_ < std::chrono::seconds(1)) return;
        availabilityChecked_ = now;
        availabilityFiles_ = fileChanges;
        libraryVersion_ = version;
        std::vector<uint64_t> deleted;
        if (const auto deletions = libraryDeletions(reader); deletions != libraryDeletions_) {
            libraryDeletions_ = deletions;
            auto exists = reader.prepare("SELECT 1 FROM files WHERE id=?");
            for (const auto& [id, binding] : libraryEntries_) {
                exists.reset();
                if (binding.file && !exists.bind(1, binding.file).step()) deleted.push_back(id);
            }
            if (!deleted.empty()) {
                removeQueue(deleted);
                queueChanged_ = true;
            }
        }
        std::vector<std::wstring> removed;
        auto roots = reader.prepare("SELECT path FROM roots WHERE removed IS NOT NULL");
        while (roots.step()) {
            auto path = toWide(roots.text(0));
            if (!path.ends_with(L'\\')) path.push_back(L'\\');
            removed.push_back(std::move(path));
        }
        // A large queue reads the unavailable files once instead of asking about each entry, which kept the worker busy
        // for seconds on every change of a 100k-entry queue.
        std::unordered_set<int64_t> gone;
        if (large) {
            auto files = reader.prepare("SELECT f.id FROM files f JOIN roots r ON r.id=f.root_id "
                                        "WHERE f.available=0 OR r.online=0 OR r.removed IS NOT NULL");
            while (files.step()) gone.insert(files.integer(0));
        }
        auto file = reader.prepare("SELECT f.available,r.online,r.removed FROM files f "
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
        discardPreloads([&](uint64_t id) { return policy_.unavailable(id); });
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
    // set once, when the entry is first opened. The scan stored it, so a share is asked only for a file it did not: a
    // NAS busy reading the playing file took most of a second to answer, before the next track could open.
    const TrackSource& playable(uint64_t id) {
        auto& queued = *std::find_if(entries_.begin(), entries_.end(), [&](const QueueEntry& e) { return e.id == id; });
        const auto binding = libraryEntries_.find(id);
        if (binding != libraryEntries_.end() && binding->second.file && !binding->second.spelled) {
            binding->second.spelled = true;
            auto spelled = libraryReader_ ? libraryStoredPath(*libraryReader_, binding->second.file) : std::nullopt;
            if (!spelled) spelled = libraryDisplayPath(queued.source.pathUtf8());
            queued.source = TrackSource(*spelled, queued.source.range(), queued.source.decoder());
            queueChanged_ = true;
        }
        return queued.source;
    }

    // Ends of the previous entry that are still pending belong to playback this call supersedes.
    // `afterEnd`: the previous entry ended naturally. Changing to another entry otherwise is manual and crossfades while
    // crossfade is on (docs/Plan.md §5.4); restarting the same entry does not.
    void startEntry(const QueuePosition& position, bool play, bool afterEnd = false) {
        if (policy_.unavailable(position.entryId)) throw std::invalid_argument("queue entry is unavailable");
        restored_.reset();
        if (!afterEnd) failuresInARow_ = 0;
        engine_->waitIdle();
        const auto before = engine_->snapshot();
        seenTrackEnds_ = before.trackEnds;
        seenTransitions_ = before.transitions;
        chain_.clear();
        const auto& source = playable(position.entryId);
        const std::filesystem::path path(toWide(source.pathUtf8()));
        shareLibraryLength(position.entryId);
        const bool manualChange = !afterEnd && state_.queueEntryId && state_.queueEntryId != position.entryId;
        state_.track = source;
        state_.queueEntryId = position.entryId;
        settings_.lastFile = path.wstring();
        if (play && manualChange && settings_.crossfade.enabled)
            engine_->crossfadeTo(path, source.decoder(), gainFor(position.entryId));
        else if (play)
            engine_->openAndPlay(path, source.decoder(), gainFor(position.entryId), afterEnd);
        else
            engine_->load(path, source.decoder(), gainFor(position.entryId));
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

    // mpg123 scans the whole stream while it plays, so its length, once known, replaces a scan-time estimate.
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
        // Opening the next file while mpg123 scans this one would wait behind the scan's reads, and this thread with it,
        // which holds back every command: it waits for the scan, or for the last seconds of the track.
        if (playback.decoder == audio::DecoderKind::Mpg123 && playback.source && !playback.source->totalFrames) {
            constexpr uint64_t kLastSeconds = 20;
            const uint64_t left = playback.total > playback.position ? playback.total - playback.position : 0;
            if (left > kLastSeconds * playback.source->sampleRate) return;
        }
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
        const auto from = chain_.empty() ? state_.queueEntryId : chain_.back();
        shareLibraryLength(upcoming->entryId);
        const std::filesystem::path file(toWide(source.pathUtf8()));
        if (engine_->queueNext(file, source.decoder(), gainFor(upcoming->entryId), crossfadeMs(from, upcoming->entryId))) {
            chain_.push_back(upcoming->entryId);
            std::lock_guard lock(readsMutex_);
            queuedRead_ = file;
        }
    }

    // What playback reads, for the library's background work (docs/Plan.md §7.7): any thread may ask.
    PlaybackReads playbackReads() const {
        const auto playback = engine_->snapshot();
        PlaybackReads reads;
        if (playback.state != audio::PlayState::Playing) return reads;
        reads.files.emplace_back(playback.file);
        if (playback.queuedFiles) {
            std::lock_guard lock(readsMutex_);
            reads.files.push_back(queuedRead_);
        }
        reads.ahead = playback.ahead;
        return reads;
    }

    // The crossfade from one entry into the next (docs/Plan.md §5.4): none while an album plays in order, the next
    // entry from the same album with shuffle off.
    uint32_t crossfadeMs(uint64_t from, uint64_t to) const {
        if (!settings_.crossfade.enabled) return 0;
        const auto album = [this](uint64_t id) {
            const auto at = std::ranges::find(entries_, id, &QueueEntry::id);
            return at == entries_.end() ? int64_t{0} : at->albumId;
        };
        if (!policy_.shuffle() && album(from) != 0 && album(from) == album(to)) return 0;
        return uint32_t(std::lround(settings_.crossfade.seconds * 1000.0));
    }

    // The length the library knows for the entry's MP3, exact or estimated, which the MP3 decoders show at once and place
    // early seeks by.
    void shareLibraryLength(uint64_t entryId) {
        const auto at = std::ranges::find(entries_, entryId, &QueueEntry::id);
        if (at == entries_.end() || !at->file || !libraryReader_) return;
        try {
            auto row = libraryReader_->prepare(
                "SELECT total_frames,length_estimated FROM tracks WHERE file_id=? AND format='mp3' AND total_frames>0");
            if (row.bind(1, at->file).step())
                audio::rememberLibraryLength(toWide(at->source.pathUtf8()), uint64_t(row.integer(0)), row.integer(1) != 0);
        } catch (const std::exception& e) {
            log::warn("Library length: %s", e.what());
        }
    }

    // The ReplayGain of a queue entry from its library values (docs/Plan.md §5.9). Auto takes the album's while the
    // queue plays the album in order: a neighbor in the queue is from the same album and shuffle is off.
    double gainFor(uint64_t entryId) {
        if (settings_.replayGain.mode == ReplayGainMode::Off) return 0.0;
        const auto at = std::ranges::find(entries_, entryId, &QueueEntry::id);
        TrackLoudness loudness;
        if (at != entries_.end() && at->file && libraryReader_) {
            try {
                loudness = trackLoudness(*libraryReader_, at->file);
            } catch (const std::exception& e) {
                log::warn("ReplayGain: %s", e.what());
            }
        }
        bool album = false;
        if (at != entries_.end() && at->albumId != 0 && !policy_.shuffle()) {
            album = (at != entries_.begin() && std::prev(at)->albumId == at->albumId) ||
                    (std::next(at) != entries_.end() && std::next(at)->albumId == at->albumId);
        }
        return replayGainDb(settings_.replayGain, loudness, album);
    }

    // What the audio analysis reads from another thread: the ReplayGain and waveform settings, the decoders, the files of
    // the current and next queue entries, which it analyzes first, and the playing track and the next for their waveforms.
    void shareLoudnessInputs() {
        std::vector<int64_t> files;
        std::vector<LoudnessAnalyzer::PlayingTrack> playing;
        auto at = std::ranges::find(entries_, state_.queueEntryId, &QueueEntry::id);
        if (at == entries_.end()) at = entries_.begin();
        for (; at != entries_.end() && files.size() < 20; ++at) {
            if (at->file) files.push_back(at->file);
            if (playing.size() < 2) playing.push_back({at->file, std::filesystem::path(toWide(at->source.pathUtf8()))});
        }
        if (library_) library_->setScanSpeed(settings_.scanSpeed);
        std::lock_guard lock(loudnessMutex_);
        loudnessSettings_ = settings_.replayGain;
        loudnessDecoders_ = settings_.decoderByFormat;
        queuedFiles_ = std::move(files);
        waveformSettings_ = settings_.waveform;
        playingTracks_ = std::move(playing);
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
            ++failuresInARow_;
        } else {
            failuresInARow_ = 0;
        }
        // Autoplay gives up on tracks that keep failing, as when the output takes none of the library's formats.
        const bool autoplay = autoplayAllowed() && failuresInARow_ < kAutoplayFailuresInARow;
        if (const auto next = policy_.next(true))
            startEntry(*next, true, true);
        else if (const auto picked = autoplay && appendAutoplay() ? policy_.next(true) : std::nullopt)
            startEntry(*picked, true, true);
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
        shareLoudnessInputs();
        auto p = playback(engine_->snapshot());
        if (settings_.backend == Backend::AirPlay) {
            std::scoped_lock lock(groupMutex_);
            p.receivers = airplayStates_;
        }
        p.receiverLeft = receiverLeft_;
        if (receiverVolumeWanted()) {
            const auto level = receiverLevel();
            p.playerVolume = level.level;
            p.playerMuted = level.muted;
        } else if (const auto level = playerVolume_ ? playerVolume_->current() : std::nullopt) {
            p.playerVolume = level->level;
            p.playerMuted = level->muted;
        }
        if (restored_ && p.state == PlaybackState::Stopped && state_.track) {
            p.state = PlaybackState::Paused;
            p.pathUtf8 = state_.track->pathUtf8();
            p.position = restored_->position;
            p.total = restored_->total;
            p.source = restored_->source;
            p.format = restored_->format;
        }
        if (!completed && !queueChanged_ && p.revision == state_.playback.revision && p.position == state_.playback.position &&
            p.underruns == state_.playback.underruns && p.playerVolume == state_.playback.playerVolume &&
            p.playerMuted == state_.playback.playerMuted && p.receivers == state_.playback.receivers &&
            p.receiverLeft == state_.playback.receiverLeft && state_.devices.revision == published_.devices.revision)
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
        if (discord_) discord_->observe(event.snapshot);
    }

    void refreshDevices() {
        DeviceCatalog catalog;
        catalog.wasapi = devices(audio::listWasapiDevices());
        catalog.asio = devices(audio::listAsioDrivers());
        catalog.ks = devices(audio::listKsPins());
        catalog.defaultWasapi = audio::defaultWasapiDevice();
        findNetworkPlayers(catalog);
        findAirPlayReceivers(catalog);
        catalog.revision = state_.devices.revision + 1;
        state_.devices = std::move(catalog);
        const auto outputs = settings_.outputs;
        const auto followed = settings_.followedDefault;
        syncOutputs(settings_, state_.devices);
        if (settings_.outputs == outputs && settings_.followedDefault == followed) return;
        engine_->applySettings(settings_);
        persist();
    }

    // Searching needs nothing from the firewall. Listening for announcements waits for the rule, so that a user who has
    // no network players never sees Windows ask about CamelPlayer.
    void findNetworkPlayers(DeviceCatalog& catalog) {
        std::scoped_lock lock(finderMutex_);
        if (finder_) {
            finder_->search();
        } else {
            FinderOptions options = finderOptions_;
            if (!firewallRuleExists()) options.notifyPort = 0;
            finder_ = std::make_unique<PlayerFinder>(options, [this] {
                devicesChangedAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
            });
        }
        for (const auto& player : finder_->players())
            catalog.network.push_back({player.description.udn, player.description.name});
    }

    void findAirPlayReceivers(DeviceCatalog& catalog) {
        if (airplayFinder_) {
            airplayFinder_->search();
        } else {
            std::scoped_lock lock(finderMutex_);
            airplayFinder_ = std::make_unique<AirPlayFinder>(airplayFinderOptions_, [this] {
                devicesChangedAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
            });
        }
        for (const auto& receiver : airplayFinder_->receivers())
            catalog.airplay.push_back({receiver.id, receiver.name, airplay::supportId(receiver.support), receiver.speakers});
    }

    void checkEnabledReceivers(const std::vector<SavedOutput>& before) {
        if (!airplayFinder_) return;
        for (const auto& output : settings_.outputs) {
            const auto same = [&](const SavedOutput& o) { return o.backend == output.backend && o.id == output.id && o.enabled; };
            if (output.backend == Backend::AirPlay && output.enabled && std::ranges::none_of(before, same))
                airplayFinder_->check(output.id);
        }
    }

    // The receivers of the group in use as the output asks for them, from any thread (AirPlayOutput.h).
    std::unique_ptr<audio::IOutput> airplayOutput(const Settings&) {
        {
            std::scoped_lock lock(groupMutex_);
            if (std::ranges::none_of(airplayMembers_, [](const AirPlayMember& m) { return m.receiver.has_value(); }))
                throw airplayRefused_ ? audio::Error(audio::ErrorCode::FormatNotSupported, "the receiver refuses this connection")
                                      : audio::Error(audio::ErrorCode::DeviceNotFound, "the AirPlay receiver is not connected");
        }
        AirPlayOutputOptions options;
        options.details = [this](const std::wstring& file) { return trackDetails(file); };
        options.members = [this] {
            std::scoped_lock lock(groupMutex_);
            return airplayMembers_;
        };
        options.memberLeft = [this](const std::string& id, const audio::Error& why) {
            std::scoped_lock lock(groupMutex_);
            airplayLeft_.emplace_back(id, why);
        };
        return createAirPlayGroupOutput(std::move(options));
    }

    bool receiverVolumeWanted() const {
        return settings_.backend == Backend::AirPlay && settings_.volume.mode == VolumeMode::Player;
    }

    AirPlayLevel levelOf(const std::string& id) const {
        const auto found = settings_.airplayLevels.find(id);
        return found == settings_.airplayLevels.end() ? AirPlayLevel{} : found->second;
    }

    std::vector<std::string> airplayGroup() const {
        if (settings_.backend != Backend::AirPlay) return {};
        return settings_.airplayGroup.empty() ? std::vector{settings_.airplay.deviceId} : settings_.airplayGroup;
    }

    // A group's level is its loudest receiver's, muted when every receiver is.
    AirPlayLevel receiverLevel() const {
        const auto group = airplayGroup();
        if (group.size() < 2) return levelOf(settings_.airplay.deviceId);
        AirPlayLevel loudest{0, true};
        for (const auto& id : group) {
            const auto level = levelOf(id);
            loudest.level = std::max(loudest.level, level.level);
            loudest.muted = loudest.muted && level.muted;
        }
        return loudest;
    }

    // The group's slider moves every receiver in proportion, and its mute mutes or unmutes them all. The proportions
    // are taken from the levels before the slider started moving, so the many steps of one drag round only once.
    void changeReceiverLevel(const std::function<void(AirPlayLevel&)>& change) {
        const AirPlayLevel was = receiverLevel();
        AirPlayLevel wanted = was;
        change(wanted);
        const auto now = std::chrono::steady_clock::now();
        if (!groupBaseline_ || now - groupBaseline_->at > kGroupDragSettles) groupBaseline_ = GroupBaseline{was.level, {}, now};
        groupBaseline_->at = now;
        for (const auto& id : airplayGroup()) {
            AirPlayLevel& level = settings_.airplayLevels.try_emplace(id).first->second;
            const uint32_t base = groupBaseline_->levels.try_emplace(id, level.level).first->second;
            if (wanted.level != was.level)
                level.level = groupBaseline_->master
                                  ? std::min(100u, uint32_t(std::lround(double(base) * wanted.level / groupBaseline_->master)))
                                  : wanted.level;
            if (wanted.muted != was.muted) level.muted = wanted.muted;
        }
        persist();
    }

    std::string receiverName(const std::string& id, const std::vector<airplay::Receiver>& found) const {
        const auto saved = std::ranges::find_if(
            settings_.outputs, [&](const SavedOutput& o) { return o.backend == Backend::AirPlay && o.id == id; });
        if (saved != settings_.outputs.end() && !saved->alias.empty()) return saved->alias;
        const auto receiver = std::ranges::find(found, id, &airplay::Receiver::id);
        if (receiver != found.end()) return receiver->name;
        return saved != settings_.outputs.end() && !saved->name.empty() ? saved->name : id;
    }

    // What the output reads from any thread: the group's receivers as found, their channels and levels. A receiver
    // that left while the others play on leaves the group, which says why.
    void followAirPlayGroup() {
        std::vector<std::pair<std::string, audio::Error>> left;
        {
            std::scoped_lock lock(groupMutex_);
            left.swap(airplayLeft_);
        }
        std::vector<airplay::Receiver> found;
        {
            std::scoped_lock lock(finderMutex_);
            if (airplayFinder_) found = airplayFinder_->receivers();
        }
        for (const auto& [id, why] : left) {
            if (std::ranges::find(settings_.airplayGroup, id) == settings_.airplayGroup.end() ||
                settings_.airplayGroup.size() < 2)
                continue;
            receiverLeft_ =
                ReceiverLeft{receiverLeft_ ? receiverLeft_->serial + 1 : 1, receiverName(id, found), why.code(), why.what()};
            setInAirPlayGroup(settings_, id, false, state_.devices);
            engine_->applySettings(settings_);
            persist();
        }
        std::vector<AirPlayMember> members;
        std::vector<ReceiverState> states;
        bool refused = false;
        const bool player = receiverVolumeWanted();
        for (const auto& id : airplayGroup()) {
            AirPlayMember member{id};
            const auto receiver = std::ranges::find(found, id, &airplay::Receiver::id);
            if (receiver != found.end()) {
                if (receiver->support == airplay::Support::Supported)
                    member.receiver = *receiver;
                else
                    refused = true;
            }
            member.channels = airplayChannels(settings_, id);
            const AirPlayLevel level = levelOf(id);
            if (player) member.volumeDb = airplay::volumeDb(level.level, level.muted);
            states.push_back({id, receiverName(id, found), level.level, level.muted, member.channels});
            members.push_back(std::move(member));
        }
        std::scoped_lock lock(groupMutex_);
        airplayMembers_ = std::move(members);
        airplayStates_ = std::move(states);
        airplayRefused_ = refused;
    }

    std::unique_ptr<audio::IOutput> networkOutput(const Settings& settings) {
        std::optional<upnp::PlayerDescription> player;
        {
            std::scoped_lock lock(finderMutex_);
            if (finder_)
                for (auto& found : finder_->players())
                    if (found.description.udn == settings.network.udn) player = std::move(found.description);
        }
        if (!player) throw audio::Error(audio::ErrorCode::DeviceNotFound, "the network player is not connected");
        NetworkOutputOptions options;
        options.gapless = settings.network.gapless;
        return createNetworkOutput(
            *player, [this](const std::wstring& file) { return trackDetails(file); },
            [this](const PlayerEvent& event) { followPlayer(event); }, options);
    }

    // The chosen player's own volume is followed from when it is chosen, whether it plays or not.
    void followPlayerVolume() {
        const std::string udn =
            settings_.backend == Backend::Network && settings_.volume.mode == VolumeMode::Player ? settings_.network.udn : "";
        if (udn == volumeUdn_ && state_.devices.revision == volumeDevices_) return;
        volumeUdn_ = udn;
        volumeDevices_ = state_.devices.revision;
        std::optional<upnp::PlayerDescription> player;
        if (!udn.empty()) {
            std::scoped_lock lock(finderMutex_);
            if (finder_)
                for (auto& found : finder_->players())
                    if (found.description.udn == udn) player = std::move(found.description);
        }
        if (player == volumePlayer_) return;
        playerVolume_.reset();
        volumePlayer_ = std::move(player);
        if (volumePlayer_ && volumePlayer_->service("RenderingControl"))
            playerVolume_ = std::make_unique<PlayerVolume>(*volumePlayer_);
    }

    TrackDetails trackDetails(const std::wstring& file) {
        TrackDetails details;
        const std::string path = toUtf8(file);
        const auto now = snapshot();
        const auto entry =
            std::ranges::find_if(now.queue.entries, [&](const QueueEntry& e) { return e.source.pathUtf8() == path; });
        if (entry != now.queue.entries.end()) {
            details.title = entry->title;
            details.artist = entry->artist;
            details.album = entry->album;
        }
        if (details.title.empty()) details.title = toUtf8(std::filesystem::path(file).stem().wstring());
        try {
            if (library_) {
                ArtworkRequest request;
                request.kind = ArtworkKind::Track;
                request.path = path;
                if (const auto cover = artwork(request, {})) {
                    std::ifstream in(cover->path, std::ios::binary);
                    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    const auto square = squarePicture(image, kPlayerCoverPixels);
                    details.coverJpeg.assign(square.begin(), square.end());
                }
            }
        } catch (const std::exception& e) {
            log::info("no cover for a network player: %s", e.what());
        }
        return details;
    }

    // What a network player did by itself, from its remote or app (docs/Plan.md §5.10).
    void followPlayer(const PlayerEvent& event) {
        Command command;
        switch (event.kind) {
        case PlayerEventKind::Playing:
            command.kind = CommandKind::Play;
            break;
        case PlayerEventKind::Paused:
            command.kind = CommandKind::Pause;
            break;
        case PlayerEventKind::NextTrack:
            command.kind = CommandKind::Next;
            break;
        case PlayerEventKind::PreviousTrack:
            command.kind = CommandKind::Previous;
            break;
        case PlayerEventKind::Seeked: {
            const auto source = snapshot().playback.source;
            if (!source) return;
            command.kind = CommandKind::Seek;
            command.frame = event.positionMs * source->sampleRate / 1000;
            break;
        }
        default:
            return;
        }
        // It runs on the output's own thread, which an exception would leave; one that comes while closing is dropped.
        try {
            enqueue(std::move(command));
        } catch (const std::exception& e) {
            log::info("network player: %s; its report is dropped", e.what());
        }
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
                const auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                while (!closing_ && queue_.empty())
                    if (cv_.wait_until(lock, due) == std::cv_status::timeout) break;
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
                    followAirPlayGroup();
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
                followPlayerVolume();
                followAirPlayGroup();
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
        playerVolume_.reset();
        {
            std::scoped_lock lock(finderMutex_);
            finder_.reset();
            airplayFinder_.reset();
        }
        if (session_) session_->persist(*library_);
        saveQueueSession(true);
        queueSaved_.set_value();
        backgroundStopped_.get_future().wait();
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
    _Guarded_by_(creditMutex_) mutable std::shared_ptr<const LibraryCreditNames> creditNames_;
    std::unique_ptr<LibraryTextIndex> libraryText_;
    std::unique_ptr<LibraryMonitor> libraryMonitor_;
    std::jthread coverWarmer_; // uses artwork_, library_ and engine_, so it stops before they go
    mutable std::mutex loudnessMutex_;
    _Guarded_by_(loudnessMutex_) ReplayGainSettings loudnessSettings_;
    _Guarded_by_(loudnessMutex_) DecoderByFormat loudnessDecoders_;
    _Guarded_by_(loudnessMutex_) std::vector<int64_t> queuedFiles_;
    _Guarded_by_(loudnessMutex_) WaveformSettings waveformSettings_;
    _Guarded_by_(loudnessMutex_) std::vector<LoudnessAnalyzer::PlayingTrack> playingTracks_;
    std::unique_ptr<LoudnessAnalyzer> loudness_; // uses the four above, library_ and engine_
    std::optional<sqlite::Database> libraryReader_;
    int64_t libraryVersion_ = 0;
    int64_t libraryDeletions_ = -1; // entries whose files left the library leave the queue
    mutable std::mutex readsMutex_;
    _Guarded_by_(readsMutex_) std::filesystem::path queuedRead_; // the file last queued in the engine to follow
    std::chrono::steady_clock::time_point availabilityChecked_;
    uint64_t availabilityFiles_ = 0;
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
    _Guarded_by_(mutex_) std::deque<Pending> queue_;
    _Guarded_by_(mutex_) CommandStamp issued_;
    _Guarded_by_(mutex_) uint64_t processed_ = 0;
    _Guarded_by_(mutex_) bool closing_ = false;
    std::promise<void> queueSaved_, backgroundStopped_;
    Snapshot published_, state_;
    QueuePolicy policy_;
    std::vector<QueueEntry> entries_;
    uint64_t lastEntryId_ = 0;
    uint64_t seenTrackEnds_ = 0;
    bool queueChanged_ = false;  // queue state changed without a command or playback change
    uint64_t autoplayTried_ = 0; // the entry whose start autoplay already extended the queue for
    static constexpr size_t kAutoplayKept = 100;
    int failuresInARow_ = 0; // tracks that failed one after another since one played or the user chose one
    static constexpr int kAutoplayFailuresInARow = 3;
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
    std::unique_ptr<DiscordPresence> discord_;
    // Held shared by other threads while they use loudness_, libraryMonitor_ or discord_; shutdown takes them away.
    mutable std::shared_mutex partsMutex_;
    static constexpr std::chrono::seconds kDevicesSettle{1};
    std::atomic<int64_t> devicesChangedAt_{0}; // steady_clock ticks of the last device event; 0 when none is pending
    FinderOptions finderOptions_;
    std::mutex finderMutex_;
    _Guarded_by_(finderMutex_) std::unique_ptr<PlayerFinder> finder_; // from the first device scan
    AirPlayFinderOptions airplayFinderOptions_;
    std::mutex groupMutex_; // the AirPlay group, as the output reads it
    _Guarded_by_(groupMutex_) std::vector<AirPlayMember> airplayMembers_;
    _Guarded_by_(groupMutex_) std::vector<ReceiverState> airplayStates_;
    _Guarded_by_(groupMutex_) bool airplayRefused_ = false;
    _Guarded_by_(groupMutex_) std::vector<std::pair<std::string, audio::Error>> airplayLeft_;
    std::optional<ReceiverLeft> receiverLeft_;
    mutable std::mutex lyricsMutex_;
    _Guarded_by_(lyricsMutex_) mutable std::string lyricsPath_;
    _Guarded_by_(lyricsMutex_) mutable Lyrics lyrics_;
    struct GroupBaseline {
        uint32_t master = 0;
        std::map<std::string, uint32_t> levels;
        std::chrono::steady_clock::time_point at;
    };
    static constexpr auto kGroupDragSettles = std::chrono::seconds(3);
    std::optional<GroupBaseline> groupBaseline_;   // the levels a moving group slider scales from
    std::unique_ptr<AirPlayFinder> airplayFinder_; // from the first device scan
    std::unique_ptr<PlayerVolume> playerVolume_;
    std::optional<upnp::PlayerDescription> volumePlayer_;
    std::string volumeUdn_;
    uint64_t volumeDevices_ = 0;
    std::thread worker_;
};

LocalServer::LocalServer(const Settings& settings, std::filesystem::path settingsFile, audio::OutputFactory factory)
    : impl_(std::make_unique<Impl>(settings, std::move(settingsFile), std::move(factory))) {}
LocalServer::~LocalServer() = default;
void LocalServer::setFinderOptions(const FinderOptions& options) {
    impl_->setFinderOptions(options);
}
void LocalServer::setAirPlayFinderOptions(const AirPlayFinderOptions& options) {
    impl_->setAirPlayFinderOptions(options);
}
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
LibraryScanStart LocalServer::scanLibraryNow(std::optional<int64_t> folder) {
    return impl_->scanLibraryNow(folder);
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
LibraryGroupPage LocalServer::browseGroups(LibraryGroupField field, bool favoritesOnly, int64_t offset, int64_t limit,
                                           std::optional<uint64_t> revision, const CancellationToken& token) const {
    return camel::server::browseGroups(impl_->library(), field, favoritesOnly, offset, limit, revision, token);
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
LibraryTrackRowPage LocalServer::browseTrackRows(TrackSort sort, bool descending, const std::optional<LibraryGroupFilter>& filter,
                                                 int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                                 const CancellationToken& token) const {
    return camel::server::browseTrackRows(impl_->library(), sort, descending, filter, offset, limit, revision, token);
}
LibraryArtistPage LocalServer::artistPage(const LibraryGroupFilter& artist, const CancellationToken& token) const {
    constexpr int64_t kSongs = 10, kAlbums = 50;
    return camel::server::artistPage(impl_->library(), artist, kSongs, kAlbums, token);
}
LibraryTrackRowPage LocalServer::smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                             const CancellationToken& token) const {
    return camel::server::smartTracks(impl_->library(), list, offset, limit, revision, token);
}
LibraryAlbumPage LocalServer::favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                             const CancellationToken& token) const {
    return camel::server::favoriteAlbums(impl_->library(), offset, limit, revision, token);
}
LibraryGroupPage LocalServer::favoriteArtists(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                              const CancellationToken& token) const {
    return camel::server::favoriteArtists(impl_->library(), offset, limit, revision, token);
}
std::optional<ArtworkReference> LocalServer::artwork(const ArtworkRequest& request, const CancellationToken& token) const {
    return impl_->artwork(request, token);
}
ArtistPicture LocalServer::artistPicture(const std::string& name, bool lookUp, const CancellationToken& token) {
    if (!lookUp) return camel::server::artistPicture(impl_->library(), name);
    const bool online = impl_->snapshot().settings.artistPictures == OnlineLookup::Allowed;
    return lookUpArtistPicture(impl_->library(), name, online, {}, token);
}
ArtistPicture LocalServer::setArtistPicture(const std::string& name, const std::vector<uint8_t>& image) {
    return camel::server::setArtistPicture(impl_->library(), name, image);
}
ArtistPicture LocalServer::removeArtistPicture(const std::string& name) {
    return camel::server::removeArtistPicture(impl_->library(), name);
}
ArtistPicture LocalServer::chooseArtistCandidate(const std::string& name, const std::string& mbid,
                                                 const CancellationToken& token) {
    if (impl_->snapshot().settings.artistPictures != OnlineLookup::Allowed)
        throw std::logic_error("online artist pictures are off");
    return camel::server::chooseArtistCandidate(impl_->library(), name, mbid, {}, token);
}

LibraryAlbumDetail LocalServer::albumDetail(int64_t album, const CancellationToken& token) const {
    return camel::server::albumDetail(impl_->library(), album, token);
}
LibraryFolderListing LocalServer::browseFolder(std::optional<int64_t> folder, const CancellationToken& token) const {
    return camel::server::browseFolder(impl_->library(), folder, token);
}
std::vector<LibraryTrackRow> LocalServer::folderTracks(int64_t folder, const CancellationToken& token) const {
    return camel::server::folderTracks(impl_->library(), folder, token);
}
std::vector<LibraryTrackRow> LocalServer::folderTracksFrom(int64_t track, const CancellationToken& token) const {
    return camel::server::folderTracksFrom(impl_->library(), track, token);
}
std::vector<int64_t> LocalServer::fileFolders(int64_t file) const {
    return camel::server::fileFolders(impl_->library(), file);
}
std::vector<LibraryRootInfo> LocalServer::libraryRoots() const {
    return camel::server::libraryRoots(impl_->library());
}

LibraryRootContents LocalServer::libraryRootContents(int64_t root) const {
    return camel::server::libraryRootContents(impl_->library(), root);
}
LibraryScanProgress LocalServer::libraryScanProgress() const {
    return impl_->library().scanProgress();
}
LoudnessProgress LocalServer::loudnessProgress() const {
    return impl_->loudnessProgress();
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
DiscordStatus LocalServer::discordStatus() const {
    return impl_->discordStatus();
}
Lyrics LocalServer::lyrics(const std::string& pathUtf8) const {
    return impl_->lyrics(pathUtf8);
}
std::optional<std::vector<uint8_t>> LocalServer::waveform(const std::string& pathUtf8) const {
    return impl_->waveform(pathUtf8);
}
FileDetails LocalServer::fileDetails(const std::string& pathUtf8) const {
    return impl_->fileDetails(pathUtf8);
}
void LocalServer::setDiscordTexts(const DiscordTexts& texts) {
    impl_->setDiscordTexts(texts);
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
