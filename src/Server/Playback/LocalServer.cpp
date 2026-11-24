// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/FileDetails.h"
#include "Server/Library/Waveform.h"
#include "Server/Library/Lyrics.h"
#include "Server/Playback/LocalServer.h"
#include <fstream>

#include "Server/Network/AirPlayOutput.h"
#include "Server/Network/AirPlayPairings.h"
#include "Server/Network/Rtsp.h"
#include "Server/Network/Firewall.h"
#include "Server/Network/HQPlayer.h"
#include "Server/Network/HQPlayerOutput.h"
#include "Server/Network/NetworkOutput.h"
#include "Server/Network/PlayerFinder.h"
#include "Server/Network/HQPlayerVolume.h"
#include "Server/Network/PlayerVolume.h"
#include "Server/Playback/Outputs.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cwchar>
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
#include "Server/Api/Caller.h"
#include "Server/Support/DiagnosticBundle.h"
#include "Server/Support/ServerFolders.h"

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
           kind == CommandKind::RemoveLibraryRoot || kind == CommandKind::DeleteOutput || kind == CommandKind::Transfer ||
           kind == CommandKind::GroupZones;
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

// The parts of the settings `sent` that read back otherwise, for the log to tell which.
std::string partsReadBackOtherwise(const SettingsDto& sent, const SettingsDto& back) {
    std::string parts;
    const auto part = [&](bool same, const std::string& name) {
        if (!same) parts += (parts.empty() ? "" : ", ") + name;
    };
    part(sent.wasapi == back.wasapi, "wasapi");
    part(sent.asio == back.asio, "asio");
    part(sent.ks == back.ks, "ks");
    part(sent.network == back.network, "network");
    part(sent.airplay == back.airplay, "airplay");
    part(sent.hqplayer == back.hqplayer, "hqplayer");
    part(sent.volume == back.volume, "volume");
    part(sent.eq == back.eq && sent.crossfeed == back.crossfeed && sent.convolution == back.convolution &&
             sent.dspOrder == back.dspOrder && sent.replayGain == back.replayGain,
         "dsp");
    part(sent.crossfade == back.crossfade, "crossfade");
    part(sent.outputs.size() == back.outputs.size(), "outputs");
    for (size_t i = 0; i < std::min(sent.outputs.size(), back.outputs.size()); ++i)
        part(sent.outputs[i] == back.outputs[i], "output " + std::to_string(i) + " (" + backendId(sent.outputs[i].backend) + ")");
    return parts.empty() ? "other" : parts;
}

void validate(const Command& c) {
    if (c.apiVersion < kApiOldest || c.apiVersion > kApiVersion || c.kind > CommandKind::GroupZones)
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
        (c.playerVolume && *c.playerVolume > 100) ||
        c.receiver.empty() == (c.kind == CommandKind::SetReceiverVolume || c.kind == CommandKind::ShowAirPlayCode ||
                               c.kind == CommandKind::PairAirPlay) ||
        c.code.empty() == (c.kind == CommandKind::PairAirPlay) ||
        c.playerMute.has_value() != (c.kind == CommandKind::SetPlayerMute) ||
        (c.kind == CommandKind::RemoveLibraryRoot ? c.rootId <= 0 : c.rootId != 0) || c.tracks.empty() == trackKeys ||
        c.path.empty() == (c.kind == CommandKind::AddLibraryRoot) ||
        c.albums.empty() == (c.kind == CommandKind::SetAlbumFavorite) ||
        c.artists.empty() == (c.kind == CommandKind::SetArtistFavorite) ||
        (c.favorite && c.kind != CommandKind::SetFavorite && c.kind != CommandKind::SetAlbumFavorite &&
         c.kind != CommandKind::SetArtistFavorite) ||
        (!c.zone.empty() && !actsOnZone(c.kind) && c.kind != CommandKind::DeleteOutput && c.kind != CommandKind::GroupZones) ||
        ((c.kind == CommandKind::DeleteOutput || c.kind == CommandKind::GroupZones) && c.zone.empty()) ||
        c.target.empty() == (c.kind == CommandKind::Transfer) || (!c.zones.empty() && c.kind != CommandKind::GroupZones))
        throw std::invalid_argument("invalid Server command payload");
    if (std::ranges::any_of(c.artists, [](const LibraryGroupFilter& a) {
            return (a.field != LibraryGroupField::Artist && a.field != LibraryGroupField::Composer) || a.value.empty() ||
                   a.role != LibraryAlbumRole::Any;
        }))
        throw std::invalid_argument("invalid favorite artist");
    const auto ranged = [](const TrackSource& source) { return source.range() != FrameRange{}; };
    if ((c.source && ranged(*c.source)) || std::any_of(c.sources.begin(), c.sources.end(), ranged))
        throw std::invalid_argument("track ranges require the future segment playback adapter");
    if (c.settings) {
        const auto back = settingsDto(fromJson(toJson(nativeSettings(*c.settings))));
        if (back != *c.settings)
            throw std::invalid_argument("invalid Server settings: " + partsReadBackOtherwise(*c.settings, back));
    }
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
    p.waitingForPlayer = s.outputStatus.waitingForPlayer;
    p.error = s.error;
    p.errorDetail = s.errorDetail;
    p.revision = s.revision;
    return p;
}

std::vector<DeviceInfo> devices(const std::vector<audio::DeviceEntry>& entries) {
    std::vector<DeviceInfo> result;
    result.reserve(entries.size());
    for (const auto& e : entries) result.push_back({e.id, e.name, {}, {}, {}, e.container});
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

bool actsOnZone(CommandKind kind) {
    switch (kind) {
    case CommandKind::OpenAndPlay:
    case CommandKind::Load:
    case CommandKind::Play:
    case CommandKind::Pause:
    case CommandKind::TogglePause:
    case CommandKind::Stop:
    case CommandKind::Seek:
    case CommandKind::ShowControlPanel:
    case CommandKind::QueueReplace:
    case CommandKind::QueueReorder:
    case CommandKind::QueueSelect:
    case CommandKind::Next:
    case CommandKind::Previous:
    case CommandKind::SetShuffle:
    case CommandKind::SetRepeat:
    case CommandKind::QueueAdd:
    case CommandKind::QueuePlayNext:
    case CommandKind::QueueRemove:
    case CommandKind::QueueClear:
    case CommandKind::SetPlayerVolume:
    case CommandKind::SetPlayerMute:
    case CommandKind::SetReceiverVolume:
    case CommandKind::Transfer:
        return true;
    default:
        return false;
    }
}

std::string zoneId(Backend backend, const std::string& output) {
    return backendId(backend) + std::string(":") + output;
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
    struct Zone;
    struct AirPlayGroup;

public:
    _No_competing_thread_ Impl(Settings settings, std::filesystem::path settingsFile, audio::OutputFactory factory)
        : settings_(std::move(settings)), settingsFile_(std::move(settingsFile)),
          factory_([this, given = std::move(factory)](const Settings& s) {
              return s.backend == Backend::Network    ? networkOutput(s)
                     : s.backend == Backend::AirPlay  ? airplayOutput(s, std::make_shared<AirPlayGroup>())
                     : s.backend == Backend::HQPlayer ? hqplayerOutput(s)
                                                      : given(s);
          }),
          airplayPairings_(std::make_shared<AirPlayPairings>(
              settingsFile_.empty() ? std::filesystem::path() : settingsFile_.parent_path() / L"airplay-pairings.dat")) {
        syncZones();
        for (auto& zone : zones_) refreshZoneState(*zone);
        collectZones();
        state_.settings = settingsDto(settings_);
        published_ = state_;
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
            // Covers being shown take the storage first: scans and fingerprints wait while one is read.
            options.coversShown = [this] { return coversInFlight_.load() > 0; };
            options.playback = [this] { return playbackReads(); };
            libraryMonitor_ = std::make_unique<LibraryMonitor>(*library_, std::move(options));
            // Album-wall thumbnails are made ahead of display while nothing plays or shows covers: every 30 s, and
            // soon after albums are written, so a scan's albums have theirs made during it, one at a time.
            coverWarmer_ = std::jthread([this](std::stop_token stop) {
                int64_t swept = -1;
                auto albumsSeen = library_->albumChanges();
                const auto busy = [this, &stop] {
                    return stop.stop_requested() || anyZonePlaying() || coversInFlight_.load() > 0;
                };
                while (!stop.stop_requested()) {
                    for (int i = 0; i < 300 && !stop.stop_requested() && library_->albumChanges() == albumsSeen; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    albumsSeen = library_->albumChanges();
                    if (busy()) continue;
                    const bool scanning = library_->scanProgress().scans > 0;
                    if (!scanning) library_->backupIfDue();
                    try {
                        // The thumbnails of covers whose files left the library go with them.
                        auto reader = library_->openReader();
                        if (const auto deletions = libraryDeletions(reader); !scanning && deletions != swept) {
                            swept = deletions;
                            if (const auto forgotten = artwork_->forgetDeleted(*library_))
                                log::info("artwork: %zu thumbnails of deleted covers removed", forgotten);
                        }
                        if (const auto made = artwork_->prepare(*library_, busy, {}, scanning ? 1 : 4))
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
            loudness.coversShown = [this] { return coversInFlight_.load() > 0; };
            loudness.waveform = [this] {
                std::lock_guard lock(loudnessMutex_);
                return waveformSettings_;
            };
            loudness.playing = [this] {
                std::lock_guard lock(loudnessMutex_);
                return playingTracks_;
            };
            loudness_ = std::make_unique<LoudnessAnalyzer>(*library_, std::move(loudness));
            pictureFinder_ = std::jthread([this](const std::stop_token& stop) { findArtistPicturesInBackground(stop); });
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
            settings_.adoptQueueChoices = false; // no saved queue had any
            for (auto& zone : zones_) zone->sessionRestored = true;
            sessionsRestored_ = true;
            Command restore;
            restore.kind = CommandKind::Load;
            restore.source.emplace(toUtf8(settings_.lastFile));
            enqueue(std::move(restore), true);
        } else {
            settings_.adoptQueueChoices = false;
            for (auto& zone : zones_) zone->sessionRestored = true;
            sessionsRestored_ = true;
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
        deferredWorker_ = std::thread([this] { deferredMain(); });
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
            observers_.clear();
        }
        cv_.notify_one();
        const bool working = worker_.joinable();
        if (working) queueSaved_.get_future().wait();
        release(loudness_);
        release(libraryMonitor_);
        coverWarmer_ = {};
        pictureFinder_ = {};
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
            c.kind == CommandKind::ApplySettings || c.kind == CommandKind::DeleteOutput || c.kind == CommandKind::Transfer ||
            c.kind == CommandKind::GroupZones)
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
        if (std::ranges::any_of(input.snapshot.zones,
                                [](const ZoneSnapshot& z) { return z.playback.state != PlaybackState::Stopped; })) {
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

    uint64_t addObserver(std::function<void(const Event&)> observer) {
        std::lock_guard lock(mutex_);
        observers_[++lastObserver_] = std::move(observer);
        return lastObserver_;
    }
    void removeObserver(uint64_t id) {
        std::lock_guard lock(mutex_);
        observers_.erase(id);
    }

    // The listener and the observers an event goes to, taken under mutex_ and told outside it.
    std::vector<std::function<void(const Event&)>> audience() const _Requires_lock_held_(mutex_) {
        std::vector<std::function<void(const Event&)>> out;
        out.reserve(observers_.size() + 1);
        if (listener_) out.push_back(listener_);
        for (const auto& [id, observer] : observers_) out.push_back(observer);
        return out;
    }

    void waitIdle() {
        std::unique_lock lock(mutex_);
        const auto target = issued_.id;
        while (processed_ < target || deferredOpen_ > 0) idle_.wait(lock);
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
        {
            std::shared_lock parts(partsMutex_);
            if (libraryMonitor_) libraryMonitor_->rescanAll();
        }
        std::lock_guard lock(mutex_);
        rebuiltBy_ = currentCaller();
        cv_.notify_all();
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
    void setHQPlayerFinderOptions(const hqplayer::FinderOptions& options) { hqplayerFinderOptions_ = options; }
    void setLocalDevices(std::function<LocalDevices()> list) { localDevices_ = std::move(list); }
    BackgroundPace backgroundPace(const std::filesystem::path& storage) const {
        return server::backgroundPace(playbackReads(), storage);
    }

    // Artist pictures found in the background (docs/Plan.md §7.7): a pass soon after the start, then whenever albums
    // change or online lookups become allowed, and an hour after the last. Each name waits while a page looks its own up,
    // a scan runs or covers are being shown, and yields to playback on its storage.
    void findArtistPicturesInBackground(const std::stop_token& stop) {
        CancellationToken token;
        const std::stop_callback cancel(stop, [&token] { token.cancel(); });
        const auto allowed = [this] { return snapshot().settings.artistPictures == OnlineLookup::Allowed; };
        ArtistPictureJob job;
        job.online = allowed;
        job.waiting = [this] {
            return pageLookups_.load() > 0 || coversInFlight_.load() > 0 || library_->scanProgress().scans > 0;
        };
        job.pace = [this](const std::filesystem::path& track) { return backgroundPace(track); };
        auto albums = library_->albumChanges();
        bool online = allowed();
        std::mutex idle;
        std::condition_variable_any stopped;
        for (auto next = std::chrono::steady_clock::now() + std::chrono::seconds(30); !stop.stop_requested();) {
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < next && library_->albumChanges() == albums &&
                   allowed() == online) {
                std::unique_lock lock(idle);
                stopped.wait_for(lock, stop, std::chrono::seconds(2), [] { return false; });
            }
            if (stop.stop_requested()) break;
            albums = library_->albumChanges();
            online = allowed();
            const auto started = std::chrono::steady_clock::now();
            try {
                const auto counts = findArtistPictures(*library_, job, token);
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
                if (counts.found + counts.ambiguous + counts.none)
                    log::info("artist pictures: %zu found, %zu left to choose, %zu without one, in %lld s", counts.found,
                              counts.ambiguous, counts.none, static_cast<long long>(seconds.count()));
            } catch (const std::exception& e) {
                log::warn("artist pictures: %s", e.what());
            }
            next = std::chrono::steady_clock::now() + std::chrono::hours(1);
        }
    }

    // Held while an artist page's own picture lookup goes on, which the background's next name waits for.
    [[nodiscard]] auto pageLookup() {
        ++pageLookups_;
        return wil::scope_exit([this] { --pageLookups_; });
    }

    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token) const {
        coversInFlight_.fetch_add(1);
        const auto done = wil::scope_exit([this] { coversInFlight_.fetch_sub(1); });
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
    PlaylistChange saveQueueAsPlaylist(std::string name, const std::string& zone) {
        auto& store = library();
        const auto now = snapshot();
        const auto shown = std::ranges::find(now.zones, zone.empty() ? now.selectedZone : zone, &ZoneSnapshot::id);
        if (shown == now.zones.end()) throw std::invalid_argument("no such zone: " + zone);
        const auto& queue = shown->queue;
        auto reader = store.openReader();
        auto file = reader.prepare("SELECT id FROM files WHERE path=? AND available=1");
        std::vector<PlaylistEntry> entries;
        for (const auto& entry : queue.entries) {
            PlaylistEntry item{std::nullopt, entry.source.pathUtf8()};
            // A library entry carries its file, which asks nothing of a storage that may be away or slow.
            if (entry.file) {
                item.track = LibraryTrackKey{entry.file};
                entries.push_back(std::move(item));
                continue;
            }
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
        settingsDue_.reset();
        if (settingsFile_.empty()) return;
        try {
            saveSettings(settings_, settingsFile_);
        } catch (const std::exception& e) {
            state_.commandError = Failure{audio::ErrorCode::Other, e.what()};
        }
    }

    // Levels and the last file change at each step of a slider and each track, and a save with its flush holds the
    // command thread for about 10 ms: they are saved once they have settled.
    void persistSoon() { settingsDue_ = std::chrono::steady_clock::now() + kSettingsSettle; }

    // The zone of the output in use, which commands that name no zone act on.
    Zone& selected() const { return *selected_; }

    Zone& zoneFor(const Command& c) {
        if (c.zone.empty()) return selected();
        for (auto& zone : zones_)
            if (zone->id == c.zone) return *zone;
        throw std::invalid_argument("no such zone: " + c.zone);
    }

    Zone& zoneNamed(const std::string& id) {
        for (auto& zone : zones_)
            if (zone->id == id) return *zone;
        throw std::invalid_argument("no such zone: " + id);
    }

    // What a command is about to do to music someone may be listening to, for the devices it touches (docs/Plan.md
    // §17.8): a zone paused, stopped or given another queue, a queue moved, an output deleted or the statistics reset.
    void recordChange(const Command& c, Zone& z) {
        const auto change = [&](const char* kind, const std::string& zone = {}) {
            return ZoneChange{++changeSerial_, kind, c.device, c.deviceName, zone};
        };
        const auto state = [](const Zone& zone) { return zone.engine->snapshot().state; };
        const bool sounding = state(z) == audio::PlayState::Playing;
        switch (c.kind) {
        case CommandKind::Pause:
        case CommandKind::TogglePause:
            if (sounding) z.state.change = change("paused");
            break;
        case CommandKind::Stop:
            if (state(z) != audio::PlayState::Stopped) z.state.change = change("stopped");
            break;
        case CommandKind::QueueReplace:
        case CommandKind::OpenAndPlay:
            z.state.change = change("replaced");
            break;
        case CommandKind::Transfer:
            z.state.change = change("transferred");
            if (const auto to = std::ranges::find(zones_, c.target, [](const auto& zone) { return zone->id; });
                to != zones_.end())
                (*to)->state.change = change("replaced");
            state_.transfer = {++changeSerial_, z.id, c.target};
            break;
        case CommandKind::PauseAll:
            for (auto& zone : zones_)
                if (state(*zone) == audio::PlayState::Playing) zone->state.change = change("paused");
            break;
        case CommandKind::DeleteOutput:
            for (const auto& zone : zones_)
                if (zone->id == c.zone) state_.notice = change("deleted", zone->state.name);
            break;
        case CommandKind::ResetAllPlayStatistics:
            state_.notice = change("statisticsReset");
            break;
        case CommandKind::ApplySettings:
            if (c.reset) state_.notice = change("playbackReset");
            break;
        default:
            break;
        }
    }

    // The queue, current entry and position of `from` go to `to`, replacing its own: it plays on from the audible
    // position, or stays paused when paused; `from` is left empty and stopped, its device closed (MZ-R09). The queue's
    // shuffle and repeat become the target output's.
    void transfer(Zone& from, Zone& to) {
        if (&from == &to) throw std::invalid_argument("a zone cannot take its own queue");
        if (const Zone* holder = holderFor(to); holder && holder != &from) {
            state_.commandError = Failure{audio::ErrorCode::DeviceInUse,
                                          "in use by " + (holder->state.name.empty() ? holder->id : holder->state.name)};
            return;
        }
        from.engine->waitIdle();
        const auto playback = from.engine->snapshot();
        const bool playing = playback.state == audio::PlayState::Playing || playback.bufferingSeconds;
        std::optional<SavedTrack> at = from.q.restored;
        if (!at && playback.state != audio::PlayState::Stopped && from.q.track &&
            playback.file == toWide(from.q.track->pathUtf8()))
            at = SavedTrack{playback.position, playback.total, playback.source, playback.format};
        log::info("zone %s goes to zone %s%s", from.id.c_str(), to.id.c_str(), playing ? ", playing on" : "");
        // Its play session ends here; the target's starts as it plays.
        if (from.q.session && library_) from.q.session->persist(*library_);
        from.q.session.reset();
        for (Zone* zone : {&from, &to}) {
            zone->engine->stop();
            zone->engine->waitIdle();
            finishWrite(*zone);
            const auto stopped = zone->engine->snapshot();
            zone->seenTrackEnds = stopped.trackEnds;
            zone->seenTransitions = stopped.transitions;
            zone->chain.clear();
            zone->attempted = {};
            zone->checked = {};
            zone->queueChanged = true;
            zone->holdingSince.reset(); // stopped, it holds its device no more
        }
        if (to.q.session && library_) to.q.session->persist(*library_);
        to.q = std::move(from.q);
        from.q = ZoneQueue();
        from.q.policy.setShuffle(from.settings.shuffle);
        from.q.policy.setRepeat(from.settings.repeat);
        {
            std::lock_guard lock(readsMutex_);
            from.queuedRead.clear();
            to.queuedRead.clear();
        }
        rememberQueueChoices(to);
        // A queue that did not play stays as it was; one paused or playing opens at its audible position.
        if (!to.q.policy.current() || !at) return;
        to.q.restored = at;
        if (playing) resumeRestored(to, at->position);
    }

    // The hardware a zone plays to (docs/Plan.md §5.12): a local device by Windows' container ID, which one DAC has
    // through WASAPI and Kernel Streaming alike; an ASIO driver by the device its name names, as no container is known
    // for it; "Windows default device" by the device it follows. Any other output is a device of its own.
    std::string deviceKey(const Zone& z) const {
        const auto& d = state_.devices;
        const auto containerOf = [](const std::vector<DeviceInfo>& list, const std::string& id) {
            const auto found = std::ranges::find(list, id, &DeviceInfo::id);
            return found == list.end() ? std::string() : found->container;
        };
        std::string container;
        if (z.backend == Backend::Wasapi)
            container = containerOf(d.wasapi, z.output.empty() ? d.defaultWasapi : z.output);
        else if (z.backend == Backend::Ks)
            container = containerOf(d.ks, z.output);
        else if (z.backend == Backend::Asio)
            container = asioContainer(z.output);
        return container.empty() ? z.id : "container:" + container;
    }

    // The container of the one local device whose name holds a word of the ASIO driver's own, such as Chord in
    // "ASIO Chord 1.05"; none when no device or several do.
    std::string asioContainer(const std::string& driver) const {
        const auto& d = state_.devices;
        const auto named = std::ranges::find(d.asio, driver, &DeviceInfo::id);
        if (named == d.asio.end()) return {};
        static const std::unordered_set<std::string> kGeneric{"asio", "driver", "usb",   "audio",  "device",    "output", "pro",
                                                              "hd",   "high",   "speed", "series", "interface", "sound"};
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        };
        std::vector<std::string> words;
        std::string word;
        for (const char c : named->name + " ") {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                word += c;
                continue;
            }
            if (word.size() >= 3 && !kGeneric.contains(lower(word))) words.push_back(lower(word));
            word.clear();
        }
        std::string found;
        for (const auto* list : {&d.wasapi, &d.ks})
            for (const auto& device : *list) {
                const auto name = lower(device.name);
                if (device.container.empty() ||
                    std::ranges::none_of(words, [&](const std::string& w) { return name.find(w) != std::string::npos; }))
                    continue;
                if (!found.empty() && found != device.container) return {};
                found = device.container;
            }
        return found;
    }

    static bool holdsDevice(const Zone& z) { return z.engine->snapshot().output.has_value(); }

    // The zone holding z's device: the first to have opened it, which keeps it until it stops or lets it go after a
    // pause (docs/Plan.md §6).
    const Zone* holderFor(const Zone& z) const {
        const auto key = deviceKey(z);
        const Zone* holder = nullptr;
        for (const auto& zone : zones_) {
            if (zone.get() == &z || !zone->holdingSince || deviceKey(*zone) != key) continue;
            if (!holder || *zone->holdingSince < *holder->holdingSince) holder = zone.get();
        }
        if (holder && z.holdingSince && *z.holdingSince <= *holder->holdingSince) return nullptr;
        if (!holder)
            for (const auto& zone : zones_)
                if (zone.get() != &z && deviceKey(*zone) == key && holdsDevice(*zone)) return zone.get();
        return holder;
    }

    // Play in a zone whose device another holds plays nothing and says which zone has it (the owner, 2026-11-21).
    bool mayPlay(const Zone& z) {
        const Zone* holder = holderFor(z);
        if (!holder) return true;
        state_.commandError =
            Failure{audio::ErrorCode::DeviceInUse, "in use by " + (holder->state.name.empty() ? holder->id : holder->state.name)};
        return false;
    }

    // A zone that came to share a held device, as one following the Windows default onto it, pauses.
    void followHolding() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& zone : zones_) {
            if (!holdsDevice(*zone))
                zone->holdingSince.reset();
            else if (!zone->holdingSince)
                zone->holdingSince = now;
        }
        for (auto& zone : zones_)
            if (zone->holdingSince && zone->engine->snapshot().state == audio::PlayState::Playing && holderFor(*zone)) {
                log::info("zone %s pauses: its device is in use by zone %s", zone->id.c_str(), holderFor(*zone)->id.c_str());
                zone->engine->pause();
            }
    }

    // Every enabled output in the output menu's order, the one in use among them. An AirPlay group is one zone, under the
    // receiver it was formed with; a receiver plays in one group at a time, the group in use's first, then by that order.
    std::vector<OutputRow> wantedZones() const {
        auto rows = outputRows(settings_, state_.devices);
        std::unordered_set<std::string> grouped;
        const auto take = [&](const std::vector<std::string>& group) {
            for (size_t i = 1; i < group.size(); ++i) grouped.insert(group[i]);
        };
        if (settings_.backend == Backend::AirPlay) take(settings_.airplayGroup);
        for (const auto& row : rows)
            if (row.backend == Backend::AirPlay && row.enabled && !row.selected && !grouped.contains(row.id))
                take(withOutputParameters(settings_, Backend::AirPlay, row.id, state_.devices).airplayGroup);
        std::vector<OutputRow> wanted;
        for (auto& row : rows)
            if (row.selected || (row.enabled && (row.backend != Backend::AirPlay || !grouped.contains(row.id))))
                wanted.push_back(std::move(row));
        if (std::ranges::none_of(wanted, &OutputRow::selected)) {
            OutputRow inUse;
            inUse.backend = settings_.backend;
            inUse.id = outputInUse(settings_);
            inUse.selected = true;
            wanted.insert(wanted.begin(), std::move(inUse));
        }
        return wanted;
    }

    // A zone whose output came back under another ID goes on as that output's, its queue and saved session with it.
    void followMoves(const std::vector<OutputMove>& moves) {
        std::unique_lock zones(zonesMutex_);
        for (const auto& move : moves) {
            const auto at = [&](const std::string& output) {
                return std::ranges::find_if(zones_,
                                            [&](const auto& z) { return z->backend == move.backend && z->output == output; });
            };
            if (const auto zone = at(move.from); zone != zones_.end() && at(move.to) == zones_.end())
                rekey(**zone, move.backend, move.to);
        }
    }

    // A zone goes on as another output's, its saved session with it.
    void rekey(Zone& z, Backend backend, const std::string& output) {
        const std::string from = z.id, to = zoneId(backend, output);
        finishWrite(z);
        if (library_) library_->write([from, to](sqlite::Database& db) { moveSession(db, from, to); });
        setKey(z, backend, output);
        zonesChanged_ = true;
    }

    static void setKey(Zone& z, Backend backend, const std::string& output) {
        z.backend = backend;
        z.output = output;
        z.id = zoneId(backend, output);
        z.state.id = z.id;
        z.state.backend = backend;
        z.state.output = output;
    }

    // Stopped, its device closed, with no queue.
    void emptyZone(Zone& z) {
        z.engine->stop();
        z.engine->waitIdle();
        if (z.q.session && library_) z.q.session->persist(*library_);
        const auto playback = z.engine->snapshot();
        z.q = ZoneQueue();
        z.chain.clear();
        z.attempted = {};
        z.checked = {};
        z.seenTrackEnds = playback.trackEnds;
        z.seenTransitions = playback.transitions;
        z.queueChanged = true;
        std::lock_guard lock(readsMutex_);
        z.queuedRead.clear();
    }

    // Brings the zones in line with the settings: a zone for each output wanted, each engine given its output's settings.
    // A change of the output in use selects that output's zone, every zone keeping its queue and playback (MZ-R07); an
    // output in use that became another, as a player back under a new identity or a receiver paired into a stereo pair,
    // takes its zone along.
    void syncZones() {
        const auto wanted = wantedZones();
        const auto& inUse = *std::ranges::find_if(wanted, &OutputRow::selected);
        std::vector<Zone*> emptied; // their queues take their outputs' shuffle and repeat
        std::vector<Zone*> created; // their saved queues are read back once the start's were
        Zone* moved = nullptr;      // its queue keeps its own, which become its new output's
        std::unique_lock zones(zonesMutex_);
        const auto wants = [&](Backend backend, const std::string& output) {
            return std::ranges::any_of(wanted, [&](const OutputRow& row) { return row.backend == backend && row.id == output; });
        };
        // The AirPlay group in use whose first receiver left goes on under the next (MZ-R08).
        const bool leftItsGroup =
            selected_ && selected_->backend == Backend::AirPlay && inUse.backend == Backend::AirPlay &&
            std::ranges::find(selected_->settings.airplayGroup, inUse.id) != selected_->settings.airplayGroup.end();
        if (selected_ && (selected_->backend != inUse.backend || selected_->output != inUse.id) &&
            (leftItsGroup || !wants(selected_->backend, selected_->output)) &&
            std::ranges::none_of(zones_, [&](const auto& z) { return z->backend == inUse.backend && z->output == inUse.id; })) {
            rekey(*selected_, inUse.backend, inUse.id);
            moved = selected_;
        }
        for (auto it = zones_.begin(); it != zones_.end();) {
            const bool kept = std::ranges::any_of(
                wanted, [&](const OutputRow& row) { return row.backend == (*it)->backend && row.id == (*it)->output; });
            if (kept) {
                ++it;
                continue;
            }
            // An output turned off keeps its zone's queue, which comes back when it is turned on.
            zones.unlock();
            saveQueueSession(**it, true);
            emptyZone(**it);
            zones.lock();
            it = zones_.erase(it);
            zonesChanged_ = true;
        }
        std::vector<std::unique_ptr<Zone>> ordered;
        ordered.reserve(wanted.size());
        for (const auto& row : wanted) {
            const auto found = std::ranges::find_if(zones_, [&](const std::unique_ptr<Zone>& z) {
                return z && z->backend == row.backend && z->output == row.id; // taken ones are left empty
            });
            Settings settings = withOutputParameters(settings_, row.backend, row.id, state_.devices);
            if (found == zones_.end()) {
                auto zone = std::make_unique<Zone>();
                setKey(*zone, row.backend, row.id);
                zone->settings = settings;
                zone->engine = std::make_unique<audio::PlaybackEngine>(
                    zone->settings, [this, group = zone->airplay](const Settings& output) {
                        return output.backend == Backend::AirPlay ? airplayOutput(output, group) : factory_(output);
                    });
                zone->state.playback = playback(zone->engine->snapshot());
                emptied.push_back(zone.get());
                created.push_back(zone.get());
                ordered.push_back(std::move(zone));
                zonesChanged_ = true;
            } else {
                (*found)->settings = std::move(settings);
                (*found)->engine->applySettings((*found)->settings);
                ordered.push_back(std::move(*found));
            }
            if (row.backend != Backend::AirPlay && ordered.back()->state.name != row.name) {
                ordered.back()->state.name = row.name;
                zonesChanged_ = true;
            }
            if (row.selected && selected_ != ordered.back().get()) {
                selected_ = ordered.back().get();
                zonesChanged_ = true;
            }
        }
        zones_ = std::move(ordered);
        nameAirPlayZones();
        zones.unlock();
        for (Zone* zone : emptied) {
            zone->q.policy.setShuffle(zone->settings.shuffle);
            zone->q.policy.setRepeat(zone->settings.repeat);
        }
        if (sessionsRestored_ && library_)
            for (Zone* zone : created) restoreQueueSession(*zone);
        if (moved) rememberQueueChoices(*moved);
    }

    // An AirPlay group is named after the receiver it was formed with and the count of the others, "Living room + 2"
    // (MZ-R24).
    void nameAirPlayZones() {
        const auto rows = outputRows(settings_, state_.devices);
        for (auto& zone : zones_) {
            if (zone->backend != Backend::AirPlay) continue;
            const auto group = airplayGroup(*zone);
            const auto row = std::ranges::find_if(
                rows, [&](const OutputRow& r) { return r.backend == Backend::AirPlay && r.id == zone->output; });
            std::string name = row == rows.end() || row->name.empty() ? zone->output : row->name;
            if (group.size() > 1) name += " + " + std::to_string(group.size() - 1);
            if (zone->state.name != name) {
                zone->state.name = std::move(name);
                zonesChanged_ = true;
            }
        }
    }

    // Deleting a saved output deletes its zone's queue, stopping a zone that plays (the owner, 2026-11-22).
    void forgetZoneQueue(const std::string& zone) {
        if (const auto found = std::ranges::find(zones_, zone, &Zone::id); found != zones_.end()) {
            Zone& z = **found;
            finishWrite(z);
            emptyZone(z);
            z.saved = {};
            z.savedRows.clear();
            z.sessionRestored = &z == selected_; // a zone going is not saved again
        }
        if (library_) library_->write([zone](sqlite::Database& db) { deleteSession(db, zone); });
    }

    // The AirPlay zone `id` plays with `members` from now on, none ungrouping it (MZ-R24). Zones group only with zones of
    // their kind, AirPlay receivers alone for now, and one playing apart is not taken.
    void groupZones(const std::string& id, const std::vector<std::string>& members) {
        Zone& z = zoneNamed(id);
        if (z.backend != Backend::AirPlay) throw std::invalid_argument("zones of this kind are not grouped");
        const auto rows = outputRows(settings_, state_.devices);
        std::vector<std::string> receivers;
        for (const auto& member : members) {
            const auto colon = member.find(':');
            Backend backend{};
            if (colon == std::string::npos || !backendFromId(std::string_view(member).substr(0, colon), backend))
                throw std::invalid_argument("no such zone: " + member);
            if (backend != z.backend) throw std::invalid_argument("zones of different kinds are not grouped");
            const std::string receiver = member.substr(colon + 1);
            if (std::ranges::none_of(rows,
                                     [&](const OutputRow& r) { return r.backend == backend && r.id == receiver && r.enabled; }))
                throw std::invalid_argument("no such zone: " + member);
            receivers.push_back(receiver);
        }
        for (const auto& other : zones_) {
            if (other.get() == &z || other->engine->snapshot().state != audio::PlayState::Playing) continue;
            const auto held = airplayGroup(*other);
            if (std::ranges::any_of(receivers, [&](const std::string& r) { return std::ranges::find(held, r) != held.end(); })) {
                state_.commandError = Failure{audio::ErrorCode::DeviceInUse,
                                              "in use by " + (other->state.name.empty() ? other->id : other->state.name)};
                return;
            }
        }
        followMoves(groupAirPlay(settings_, z.output, receivers, state_.devices));
        syncZones();
        persist();
    }

    void deleteSavedOutput(const std::string& zone) {
        const auto colon = zone.find(':');
        Backend backend{};
        if (colon == std::string::npos || !backendFromId(std::string_view(zone).substr(0, colon), backend))
            throw std::invalid_argument("no such output: " + zone);
        const auto outputs = settings_.outputs;
        forgetZoneQueue(zone);
        deleteOutput(settings_, backend, zone.substr(colon + 1), state_.devices);
        forgetDeletedReceivers(outputs);
        followAirPlayGroup();
        syncZones();
        persist();
    }

    // Before zones, the saved queue's shuffle and repeat were every output's: they become every zone's (MZ-R02).
    void adoptQueueChoices(const Zone& restored) {
        settings_.adoptQueueChoices = false;
        const bool shuffle = restored.q.policy.shuffle();
        const RepeatMode repeat = restored.q.policy.repeat();
        settings_.shuffle = shuffle;
        settings_.repeat = repeat;
        for (auto& output : settings_.outputs) {
            output.zone.shuffle = shuffle;
            output.zone.repeat = repeat;
        }
        for (auto& zone : zones_) {
            zone->settings.shuffle = shuffle;
            zone->settings.repeat = repeat;
            if (zone.get() == &restored) continue;
            zone->q.policy.setShuffle(shuffle);
            zone->q.policy.setRepeat(repeat);
        }
        persist();
    }

    // A zone's shuffle and repeat are kept with its output (MZ-R02).
    void rememberQueueChoices(Zone& z) {
        z.settings.shuffle = z.q.policy.shuffle();
        z.settings.repeat = z.q.policy.repeat();
        keepQueueChoices(settings_, z.backend, z.output, z.settings.shuffle, z.settings.repeat, state_.devices);
        persistSoon();
    }

    void resumePlayback(Zone& z) {
        if (!mayPlay(z)) return;
        if (z.q.entries.empty() && autoplayAllowed(z) && appendAutoplay(z)) {
            startEntry(z, z.q.policy.select(z.q.entries.front().id), true);
            return;
        }
        const auto current = z.q.policy.current();
        if (z.engine->snapshot().state == audio::PlayState::Stopped && !z.q.entries.empty() &&
            (!current || z.q.policy.unavailable(current->entryId))) {
            const auto next = z.q.policy.next(false);
            if (!next) throw std::invalid_argument("queue entries are unavailable");
            startEntry(z, *next, true);
        } else
            z.engine->play();
    }

    // The restored current entry opens its file only now, at the position it was left at.
    void resumeRestored(Zone& z, uint64_t position) {
        if (!mayPlay(z)) return;
        const auto current = z.q.policy.current();
        z.q.restored.reset();
        if (!current || z.q.policy.unavailable(current->entryId)) {
            resumePlayback(z);
            return;
        }
        startEntry(z, *current, false);
        if (position) z.engine->seek(position);
        z.engine->play();
    }

    // Library entries are found by their file, wherever it moved, so restoring reads only the library database
    // (docs/Plan.md §7); an unavailable file stays in the queue, marked unavailable.
    // Every zone's saved queue comes back paused (docs/Plan.md §2); the one saved before zones becomes the selected zone's.
    void restoreSessions() {
        bool selectedSaved = false;
        if (library_) {
            auto reader = library_->openReader();
            if (hasSavedSession(reader, kQueueBeforeZones) && !hasSavedSession(reader, selected().id))
                library_->write([to = selected().id](sqlite::Database& db) { moveSession(db, kQueueBeforeZones, to); }).get();
            selectedSaved = hasSavedSession(reader, selected().id);
        }
        for (auto& zone : zones_) restoreQueueSession(*zone);
        sessionsRestored_ = true;
        if (settings_.adoptQueueChoices && selectedSaved)
            adoptQueueChoices(selected());
        else
            settings_.adoptQueueChoices = false;
    }

    // Waits for the zone's last write, which a move of its rows must follow.
    static void finishWrite(Zone& z) {
        if (!z.sessionWrite.valid()) return;
        try {
            z.sessionWrite.get();
        } catch (const std::exception& e) {
            log::warn("session: %s", e.what());
        }
    }

    void restoreQueueSession(Zone& z) {
        z.sessionRestored = true;
        const auto started = measure::now();
        auto measured = wil::scope_exit([&] {
            if (measure::measuring())
                log::info("measure: queue of %zu entries restored in %.0f ms", z.q.entries.size(),
                          measure::milliseconds(started, measure::now()));
        });
        auto reader = library_->openReader();
        const auto saved = loadSession(reader, z.id);
        if (!saved) return;
        for (const auto& item : saved->entries) z.savedRows.push_back(rowHash(item));
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
            z.q.libraryEntries[entry.id] = {key, item.file, false, false, spelled};
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
        z.q.policy.restore(std::move(state));
        applyMarks(z, entries);
        z.q.entries = std::move(entries);
        if (const auto current = z.q.policy.current()) {
            z.q.entryId = current->entryId;
            z.q.track = z.q.entries[current->index].source;
            z.q.restored = saved->track;
        }
        z.q.libraryVersion = 0;
        refreshLibraryAvailability(z);
        z.queueChanged = true;
        auto policy = z.q.policy.state();
        z.saved = {std::move(policy.entries),
                   std::move(policy.order),
                   std::move(policy.history),
                   policy.current,
                   policy.shuffle,
                   policy.repeat,
                   z.q.restored.value_or(SavedTrack{}),
                   z.engine->snapshot().state,
                   std::chrono::steady_clock::now()};
    }

    SavedTrack currentTrack(const Zone& z, const audio::EngineSnapshot& playback) const {
        if (z.q.restored) return *z.q.restored;
        if (!z.q.entryId || !playback.source) return {};
        SavedTrack track{playback.position, playback.total, playback.source, playback.format};
        track.source->md5.reset();
        return track;
    }

    // Written when the queue, its order or the current entry changes, on pause and stop, every 5 seconds while playing
    // (what a crash can lose) and at exit. Large queues are rewritten only when their entries or order change.
    void saveQueueSession(Zone& z, bool final) {
        if (!library_ || !z.sessionRestored) return;
        if (z.sessionWrite.valid()) {
            if (!final && z.sessionWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try {
                z.sessionWrite.get();
            } catch (const std::exception& e) {
                log::warn("session: %s", e.what());
                z.saved = {}; // nothing of it is in the database: the next save writes it all
                z.savedRows.clear();
            }
        }
        const auto playback = z.engine->snapshot();
        const auto now = std::chrono::steady_clock::now();
        auto policy = z.q.policy.state();
        SessionParts parts{policy.entries != z.saved.entries, policy.order != z.saved.order};
        const auto track = currentTrack(z, playback);
        const bool changed =
            parts.entries || parts.order || policy.current != z.saved.current || policy.history != z.saved.history ||
            policy.shuffle != z.saved.shuffle || policy.repeat != z.saved.repeat || playback.state != z.saved.playState ||
            (playback.state == audio::PlayState::Playing ? now - z.saved.at >= std::chrono::seconds(5) : track != z.saved.track);
        if (!changed && !final) return;
        std::unordered_map<uint64_t, size_t> index;
        index.reserve(z.q.entries.size());
        for (size_t i = 0; i < z.q.entries.size(); ++i) index.emplace(z.q.entries[i].id, i);
        const auto indexes = [&](const std::vector<uint64_t>& ids) {
            std::vector<size_t> result;
            for (const auto id : ids)
                if (const auto at = index.find(id); at != index.end()) result.push_back(at->second);
            return result;
        };
        SavedSession session;
        if (parts.entries) {
            for (const auto& entry : z.q.entries) {
                const auto binding = z.q.libraryEntries.find(entry.id);
                const auto title = entry.title.empty()
                                       ? toUtf8(std::filesystem::path(toWide(entry.source.pathUtf8())).stem().wstring())
                                       : entry.title;
                session.entries.push_back({entry.source, binding == z.q.libraryEntries.end() ? 0 : binding->second.file,
                                           entry.artist.empty() ? title : title + " · " + entry.artist, entry.automatic});
            }
            std::vector<size_t> rows;
            rows.reserve(session.entries.size());
            for (const auto& row : session.entries) rows.push_back(rowHash(row));
            parts.keptEntries = size_t(std::ranges::mismatch(rows, z.savedRows).in1 - rows.begin());
            z.savedRows = std::move(rows);
        }
        if (parts.order) session.order = indexes(policy.order);
        session.history = indexes(policy.history);
        if (policy.current)
            if (const auto at = index.find(*policy.current); at != index.end()) session.current = at->second;
        session.shuffle = policy.shuffle;
        session.repeat = policy.repeat;
        session.track = track;
        auto write = library_->write(
            [zone = z.id, session = std::move(session), parts](sqlite::Database& db) { saveSession(db, zone, session, parts); });
        z.saved = {std::move(policy.entries),
                   std::move(policy.order),
                   std::move(policy.history),
                   policy.current,
                   policy.shuffle,
                   policy.repeat,
                   track,
                   playback.state,
                   now};
        if (!final) {
            z.sessionWrite = std::move(write);
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
        state_.commandError.reset();
        state_.controlPanelOpened.reset();
        // A command of no zone may add or remove zones, so it uses none after that.
        Zone& z = c.kind == CommandKind::DeleteOutput ? selected() : zoneFor(c);
        recordChange(c, z);
        z.attempted = {}; // a command may restart the stream, so the upcoming entry may be queued again
        z.checked = {};
        if (pending.restore && !c.source) {
            restoreSessions();
            return;
        }
        if (c.source) {
            if (pending.restore) {
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::path(toWide(c.source->pathUtf8())), ec)) return;
            }
            const auto ids = replaceQueue(z, {*c.source});
            startEntry(z, z.q.policy.select(ids.front()), c.kind == CommandKind::OpenAndPlay);
        } else {
            switch (c.kind) {
            case CommandKind::QueueReplace: {
                const auto ids = replaceQueue(z, c.sources, size_t(c.index));
                startEntry(z, z.q.policy.select(ids[size_t(c.index)]), true);
                break;
            }
            case CommandKind::QueueReorder:
                reorderQueue(z, c.entryIds);
                break;
            case CommandKind::QueueAdd:
                appendQueue(z, c.sources, false);
                break;
            case CommandKind::QueuePlayNext:
                appendQueue(z, c.sources, true);
                break;
            case CommandKind::QueueRemove:
                removeQueue(z, c.entryIds);
                break;
            case CommandKind::QueueClear:
                clearQueue(z);
                break;
            case CommandKind::QueueSelect:
                startEntry(z, z.q.policy.select(c.entryId), true);
                break;
            case CommandKind::Next:
                if (const auto next = z.q.policy.next(false))
                    startEntry(z, *next, true);
                else if (const auto picked = autoplayAllowed(z) && appendAutoplay(z) ? z.q.policy.next(false) : std::nullopt)
                    startEntry(z, *picked, true);
                else
                    z.engine->stop();
                break;
            case CommandKind::Previous: {
                const auto playback = z.engine->snapshot();
                const auto previous =
                    z.q.restored
                        ? z.q.policy.previous(z.q.restored->position, z.q.restored->source ? z.q.restored->source->sampleRate : 0)
                        : z.q.policy.previous(playback.position, playback.source ? playback.source->sampleRate : 0);
                if (!previous) break;
                if (previous->entryId == z.q.entryId && z.q.restored)
                    z.q.restored->position = 0;
                else if (previous->entryId == z.q.entryId)
                    z.engine->seek(0);
                else
                    startEntry(z, *previous, true);
                break;
            }
            case CommandKind::SetShuffle:
                if (c.shuffle) z.q.policy.setShuffle(*c.shuffle);
                rememberQueueChoices(z);
                break;
            case CommandKind::SetRepeat:
                if (c.repeat) z.q.policy.setRepeat(*c.repeat);
                rememberQueueChoices(z);
                break;
            case CommandKind::AddLibraryRoot:
                if (!library_) throw std::logic_error("Server library is not configured");
                defer([this, path = c.path] {
                    libraryMonitor_->scanAdded(LibraryScanner(*library_).addRoot(std::filesystem::path(toWide(path))));
                });
                break;
            case CommandKind::RemoveLibraryRoot:
                if (!library_) throw std::logic_error("Server library is not configured");
                defer([this, root = c.rootId] { LibraryScanner(*library_).removeRoot(root); },
                      [this] {
                          for (auto& zone : zones_) refreshLibraryAvailability(*zone);
                      });
                break;
            case CommandKind::Play:
                if (z.q.restored)
                    resumeRestored(z, z.q.restored->position);
                else
                    resumePlayback(z);
                break;
            case CommandKind::Pause:
                z.engine->pause();
                break;
            case CommandKind::TogglePause:
                if (z.q.restored)
                    resumeRestored(z, z.q.restored->position);
                else if (z.engine->snapshot().state == audio::PlayState::Stopped)
                    resumePlayback(z);
                else if (z.engine->snapshot().state == audio::PlayState::Playing || mayPlay(z))
                    z.engine->togglePause();
                break;
            case CommandKind::Stop:
                z.engine->stop();
                if (z.q.restored) z.q.restored->position = 0;
                break;
            case CommandKind::Seek:
                if (z.q.restored) {
                    z.q.restored->position = z.q.restored->total ? std::min(c.frame, z.q.restored->total) : c.frame;
                    break;
                }
                if (z.q.session) z.q.session->interrupt();
                z.engine->seek(c.frame);
                break;
            case CommandKind::SetPlayerVolume:
                if (z.volume)
                    z.volume->setLevel(*c.playerVolume);
                else if (receiverVolumeWanted(z))
                    changeReceiverLevel(z, [&](AirPlayLevel& level) { level.level = *c.playerVolume; });
                break;
            case CommandKind::SetPlayerMute:
                if (z.volume)
                    z.volume->setMuted(*c.playerMute);
                else if (receiverVolumeWanted(z))
                    changeReceiverLevel(z, [&](AirPlayLevel& level) { level.muted = *c.playerMute; });
                break;
            case CommandKind::SetReceiverVolume:
                if (const auto group = airplayGroup(z);
                    receiverVolumeWanted(z) && c.playerVolume && std::ranges::find(group, c.receiver) != group.end()) {
                    settings_.airplayLevels.try_emplace(c.receiver).first->second.level = std::min(*c.playerVolume, 100u);
                    z.groupBaseline.reset();
                    persistSoon();
                }
                break;
            case CommandKind::ResetPlayStatistics:
            case CommandKind::ResetAllPlayStatistics:
                if (!library_) throw std::logic_error("Server library is not configured");
                for (auto& zone : zones_)
                    if (auto& session = zone->q.session;
                        session && (c.tracks.empty() || std::ranges::find(c.tracks, session->track()) != c.tracks.end()))
                        session->discard();
                defer([this, tracks = c.tracks] { resetPlayStatistics(*library_, tracks); });
                break;
            case CommandKind::SetFavorite:
            case CommandKind::SetAlbumFavorite:
                if (!library_) throw std::logic_error("Server library is not configured");
                defer(
                    [this, track = c.kind == CommandKind::SetFavorite, tracks = c.tracks, albums = c.albums,
                     favorite = c.favorite] {
                        if (track)
                            setTrackFavorite(*library_, tracks, favorite);
                        else
                            setAlbumFavorite(*library_, albums, favorite);
                    },
                    [this] {
                        for (auto& zone : zones_) {
                            applyMarks(*zone, zone->q.entries);
                            zone->queueChanged = true;
                        }
                        ++marks_;
                    });
                break;
            case CommandKind::SetArtistFavorite:
                if (!library_) throw std::logic_error("Server library is not configured");
                defer([this, artists = c.artists, favorite = c.favorite] { setArtistFavorite(*library_, artists, favorite); });
                break;
            case CommandKind::ApplySettings: {
                if (c.settings) {
                    const auto lastFile = settings_.lastFile;
                    const bool adoptOutputSets = settings_.adoptOutputSets;
                    const bool adoptQueueChoices = settings_.adoptQueueChoices;
                    const auto queueChoices = std::pair{settings_.shuffle, settings_.repeat};
                    const auto outputs = settings_.outputs;
                    auto airplayLevels = std::move(settings_.airplayLevels);
                    settings_ = nativeSettings(*c.settings);
                    settings_.airplayLevels = std::move(airplayLevels);
                    // Shuffle and repeat are the queues', which only queue commands change.
                    std::tie(settings_.shuffle, settings_.repeat) = queueChoices;
                    for (auto& output : settings_.outputs)
                        if (const auto was = std::ranges::find_if(
                                outputs, [&](const SavedOutput& o) { return o.backend == output.backend && o.id == output.id; });
                            was != outputs.end()) {
                            output.zone.shuffle = was->zone.shuffle;
                            output.zone.repeat = was->zone.repeat;
                        }
                    settings_.adoptQueueChoices = adoptQueueChoices;
                    for (const auto& output : outputs)
                        if (std::ranges::none_of(settings_.outputs, [&](const SavedOutput& o) {
                                return o.backend == output.backend && o.id == output.id;
                            }))
                            forgetZoneQueue(zoneId(output.backend, output.id));
                    std::erase_if(settings_.airplayLevels, [&](const auto& entry) {
                        return std::ranges::none_of(settings_.outputs, [&](const SavedOutput& o) {
                            return o.backend == Backend::AirPlay && o.id == entry.first;
                        });
                    });
                    settings_.lastFile = lastFile;
                    settings_.adoptOutputSets = adoptOutputSets;
                    syncOutputs(settings_, state_.devices);
                    forgetDeletedReceivers(outputs);
                    checkEnabledReceivers(outputs);
                    followAirPlayGroup();
                    struct Fades {
                        std::string zone;
                        ReplayGainSettings gain;
                        CrossfadeSettings crossfade;
                    };
                    std::vector<Fades> fades;
                    fades.reserve(zones_.size());
                    for (const auto& zone : zones_)
                        fades.push_back({zone->id, zone->settings.replayGain, zone->settings.crossfade});
                    syncZones();
                    discord_->configure(settings_.discord);
                    shareLoudnessInputs();
                    // A queued track took the old gain and crossfade; it is queued again with the new ones. The playing
                    // one keeps its.
                    for (auto& zone : zones_) {
                        const auto was = std::ranges::find(fades, zone->id, &Fades::zone);
                        const bool changed = was != fades.end() && (was->gain != zone->settings.replayGain ||
                                                                    was->crossfade != zone->settings.crossfade);
                        if (changed && !zone->chain.empty() && zone->engine->clearNext()) {
                            zone->chain.pop_back();
                            zone->attempted = {};
                            zone->checked = {};
                        }
                    }
                    persist();
                }
                break;
            }
            case CommandKind::RefreshDevices:
                refreshDevices();
                break;
            case CommandKind::DeleteOutput:
                deleteSavedOutput(c.zone);
                break;
            case CommandKind::Transfer:
                transfer(z, zoneNamed(c.target));
                break;
            case CommandKind::GroupZones:
                groupZones(c.zone, c.zones);
                break;
            case CommandKind::PauseAll:
                for (auto& zone : zones_)
                    if (zone->engine->snapshot().state == audio::PlayState::Playing) zone->engine->pause();
                break;
            case CommandKind::ShowAirPlayCode:
                defer([this, id = c.receiver] { showAirPlayCode(id); });
                break;
            case CommandKind::PairAirPlay:
                defer([this, id = c.receiver, code = c.code] { pairAirPlay(id, code); }, [this] { refreshDevices(); });
                break;
            case CommandKind::ShowControlPanel:
                try {
                    state_.controlPanelOpened = z.engine->showControlPanel();
                } catch (const audio::Error& e) {
                    state_.controlPanelOpened = false;
                    state_.commandError = Failure{e.code(), e.what()};
                }
                break;
            default:
                break;
            }
        }
        if (actsOnZone(c.kind) || pending.restore)
            waitForEngine(z);
        else
            for (auto& zone : zones_) waitForEngine(*zone);
    }

    std::vector<uint64_t> replaceQueue(Zone& z, const std::vector<TrackSource>& sources, size_t selected = 0) {
        z.q.libraryEntries.clear();
        auto entries = buildQueueEntries(z, sources);
        std::vector<uint64_t> ids;
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.push_back(e.id);
        z.q.policy.replace(ids);
        z.q.entries = std::move(entries);
        z.q.libraryVersion = 0;
        refreshLibraryAvailability(z);
        if (z.q.policy.unavailable(ids[selected])) {
            z.engine->stop();
            z.engine->waitIdle();
            z.q.entryId = 0;
            z.q.track.reset();
            z.q.restored.reset();
            z.chain.clear();
            throw std::invalid_argument("queue entry is unavailable");
        }
        return ids;
    }

    std::vector<QueueEntry> buildQueueEntries(Zone& z, const std::vector<TrackSource>& sources) {
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
                    auto& binding = z.q.libraryEntries[id];
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
        applyMarks(z, entries);
        return entries;
    }

    // The entries' library files and their favorite marks.
    void applyMarks(Zone& z, std::vector<QueueEntry>& entries) {
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
            const auto binding = z.q.libraryEntries.find(entry.id);
            entry.file = binding == z.q.libraryEntries.end() ? 0 : binding->second.file;
            entry.favorite = entry.file && favorites.contains(entry.file);
        }
    }

    void appendQueue(Zone& z, const std::vector<TrackSource>& sources, bool playNext) {
        // The user's tracks play next, so automatic entries that have not started yet go (docs/Plan.md §8.1).
        const auto playing =
            std::find_if(z.q.entries.begin(), z.q.entries.end(), [&](const QueueEntry& e) { return e.id == z.q.entryId; });
        std::vector<uint64_t> pending;
        for (auto it = playing == z.q.entries.end() ? z.q.entries.begin() : playing + 1; it != z.q.entries.end(); ++it)
            if (it->automatic) pending.push_back(it->id);
        if (!pending.empty()) removeQueue(z, pending);
        auto entries = buildQueueEntries(z, sources);
        std::vector<uint64_t> ids;
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.push_back(e.id);
        z.q.policy.append(ids, playNext);
        auto at = z.q.entries.end();
        if (playNext)
            if (const auto current = std::find_if(z.q.entries.begin(), z.q.entries.end(),
                                                  [&](const QueueEntry& e) { return e.id == z.q.entryId; });
                current != z.q.entries.end())
                at = current + 1;
        z.q.entries.insert(at, std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
        z.q.libraryVersion = 0;
        refreshLibraryAvailability(z);
    }

    // The Engine may already decode entries about to leave the queue; it drops them while the queue still knows them,
    // so a transition already heard becomes current first.
    void discardPreloads(Zone& z, const std::function<bool(uint64_t)>& leaving) {
        if (std::ranges::none_of(z.chain, leaving)) return;
        z.engine->discardPreloads();
        commitTransition(z);
        z.chain.clear();
        z.attempted = {};
        z.checked = {};
    }

    void removeQueue(Zone& z, const std::vector<uint64_t>& ids) {
        const std::unordered_set<uint64_t> leaving(ids.begin(), ids.end());
        discardPreloads(z, [&](uint64_t id) { return leaving.contains(id); });
        z.q.policy.remove(ids);
        if (z.q.restored && leaving.contains(z.q.entryId)) {
            z.q.restored.reset();
            z.q.entryId = 0;
            z.q.track.reset();
        }
        std::erase_if(z.q.entries, [&](const QueueEntry& e) { return leaving.contains(e.id); });
        for (const auto id : ids) z.q.libraryEntries.erase(id);
    }

    void clearQueue(Zone& z) {
        discardPreloads(z, [](uint64_t) { return true; });
        const uint64_t current = z.q.entryId;
        const auto keep =
            std::find_if(z.q.entries.begin(), z.q.entries.end(), [&](const QueueEntry& e) { return e.id == current; });
        z.q.policy.clearExcept(keep == z.q.entries.end() ? std::nullopt : std::optional(current));
        if (keep == z.q.entries.end())
            z.q.entries.clear();
        else {
            QueueEntry currentEntry = *keep;
            z.q.entries = {std::move(currentEntry)};
        }
        std::erase_if(z.q.libraryEntries, [&](const auto& item) { return item.first != current; });
    }

    // `idle` passes may skip a large queue for up to a second, since a scan changes the library many times a second.
    void refreshLibraryAvailability(Zone& z, bool idle = false) {
        if (!libraryReader_ || z.q.libraryEntries.empty()) return;
        auto& reader = *libraryReader_;
        const auto version = reader.scalar("PRAGMA data_version").value();
        if (version == z.q.libraryVersion) return;
        const bool large = z.q.libraryEntries.size() > 2000;
        const auto now = std::chrono::steady_clock::now();
        // Availability comes from files and roots alone, and a large queue reads all of files: other commits, such as
        // the queue session saved every 5 s while playing, are left for the once-a-minute pass.
        const auto fileChanges = library_->fileChanges();
        if (large && z.q.libraryVersion && fileChanges == z.q.availabilityFiles &&
            now - z.q.availabilityChecked < std::chrono::minutes(1))
            return;
        if (idle && large && now - z.q.availabilityChecked < std::chrono::seconds(1)) return;
        z.q.availabilityChecked = now;
        z.q.availabilityFiles = fileChanges;
        z.q.libraryVersion = version;
        std::vector<uint64_t> deleted;
        if (const auto deletions = libraryDeletions(reader); deletions != z.q.libraryDeletions) {
            z.q.libraryDeletions = deletions;
            auto exists = reader.prepare("SELECT 1 FROM files WHERE id=?");
            for (const auto& [id, binding] : z.q.libraryEntries) {
                exists.reset();
                if (binding.file && !exists.bind(1, binding.file).step()) deleted.push_back(id);
            }
            if (!deleted.empty()) {
                removeQueue(z, deleted);
                z.queueChanged = true;
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
        for (auto& [id, binding] : z.q.libraryEntries) {
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
            z.queueChanged = z.q.policy.setUnavailable(id, unavailable) || z.queueChanged;
        }
        discardPreloads(z, [&](uint64_t id) { return z.q.policy.unavailable(id); });
    }

    void reorderQueue(Zone& z, const std::vector<uint64_t>& ids) {
        z.q.policy.reorder(ids);
        std::unordered_map<uint64_t, QueueEntry*> byId;
        byId.reserve(z.q.entries.size());
        for (auto& entry : z.q.entries) byId.emplace(entry.id, &entry);
        std::vector<QueueEntry> entries;
        entries.reserve(ids.size());
        for (const auto id : ids) entries.push_back(std::move(*byId.at(id)));
        z.q.entries = std::move(entries);
    }

    const QueueEntry& entry(const Zone& z, uint64_t id) const {
        return *std::find_if(z.q.entries.begin(), z.q.entries.end(), [&](const QueueEntry& e) { return e.id == id; });
    }

    // Library sources arrive as lowercase index keys; the file opens and shows under its on-disk spelling, which is
    // set once, when the entry is first opened. The scan stored it, so a share is asked only for a file it did not: a
    // NAS busy reading the playing file took most of a second to answer, before the next track could open.
    const TrackSource& playable(Zone& z, uint64_t id) {
        auto& queued = *std::find_if(z.q.entries.begin(), z.q.entries.end(), [&](const QueueEntry& e) { return e.id == id; });
        const auto binding = z.q.libraryEntries.find(id);
        if (binding != z.q.libraryEntries.end() && binding->second.file && !binding->second.spelled) {
            binding->second.spelled = true;
            auto spelled = libraryReader_ ? libraryStoredPath(*libraryReader_, binding->second.file) : std::nullopt;
            if (!spelled) spelled = libraryDisplayPath(queued.source.pathUtf8());
            queued.source = TrackSource(*spelled, queued.source.range(), queued.source.decoder());
            z.queueChanged = true;
        }
        return queued.source;
    }

    // Ends of the previous entry that are still pending belong to playback this call supersedes.
    // `afterEnd`: the previous entry ended naturally. Changing to another entry otherwise is manual and crossfades while
    // crossfade is on (docs/Plan.md §5.4); restarting the same entry does not.
    void startEntry(Zone& z, const QueuePosition& position, bool play, bool afterEnd = false) {
        if (play && !afterEnd && !mayPlay(z)) play = false;
        if (z.q.policy.unavailable(position.entryId)) throw std::invalid_argument("queue entry is unavailable");
        z.q.restored.reset();
        if (!afterEnd) z.q.failuresInARow = 0;
        z.engine->waitIdle();
        const auto before = z.engine->snapshot();
        z.seenTrackEnds = before.trackEnds;
        z.seenTransitions = before.transitions;
        z.chain.clear();
        const auto& source = playable(z, position.entryId);
        const std::filesystem::path path(toWide(source.pathUtf8()));
        shareLibraryLength(z, position.entryId);
        const bool manualChange = !afterEnd && z.q.entryId && z.q.entryId != position.entryId;
        z.q.track = source;
        z.q.entryId = position.entryId;
        if (&z == &selected()) settings_.lastFile = path.wstring();
        if (play && manualChange && z.settings.crossfade.enabled)
            z.engine->crossfadeTo(path, source.decoder(), gainFor(z, position.entryId));
        else if (play)
            z.engine->openAndPlay(path, source.decoder(), gainFor(z, position.entryId), afterEnd);
        else
            z.engine->load(path, source.decoder(), gainFor(z, position.entryId));
        waitForEngine(z); // the next snapshot pairs this entry with its own playback state
        persistSoon();
    }

    // A gapless transition already plays the queued entry; it becomes current without reopening anything.
    void commitTransition(Zone& z) {
        const auto playback = z.engine->snapshot();
        if (playback.transitions == z.seenTransitions) return;
        auto heard = playback.transitions - z.seenTransitions;
        z.seenTransitions = playback.transitions;
        for (; heard && !z.chain.empty(); --heard) {
            const auto id = z.chain.front();
            z.chain.pop_front();
            auto position = z.q.policy.next(true);
            if (!position || position->entryId != id) position = z.q.policy.select(id, true);
            const auto& source = entry(z, position->entryId).source;
            z.q.track = source;
            z.q.entryId = position->entryId;
            if (&z == &selected()) settings_.lastFile = toWide(source.pathUtf8());
            z.queueChanged = true;
        }
        persistSoon();
    }

    // mpg123 scans the whole stream while it plays, so its length, once known, replaces a scan-time estimate.
    void recordPlaybackLength(Zone& z) {
        if (!library_ || !z.q.entryId) return;
        const auto binding = z.q.libraryEntries.find(z.q.entryId);
        if (binding == z.q.libraryEntries.end() || !binding->second.file || binding->second.lengthRecorded) return;
        const auto playback = z.engine->snapshot();
        if (!playback.source || !playback.source->totalFrames || playback.decoder != audio::DecoderKind::Mpg123 ||
            playback.file != toWide(entry(z, z.q.entryId).source.pathUtf8()))
            return;
        binding->second.lengthRecorded = true;
        LibraryScanner(*library_).recordExactLength(binding->second.file, playback.source->sampleRate,
                                                    playback.source->totalFrames);
    }

    // A play session lasts while one library entry stays current (docs/Plan.md §7.8).
    void updatePlaySession(Zone& z) {
        if (!library_) return;
        const auto playback = z.engine->snapshot();
        const auto now = std::chrono::steady_clock::now();
        // Audio the device can have played since the previous pass; larger jumps are seeks or restarts.
        const auto reachable = [&](uint32_t rate) {
            return uint64_t((std::chrono::duration<double>(now - z.q.sessionSeen).count() * 1.25 + 0.25) * rate);
        };
        if (z.q.session) {
            const bool current = playback.transitions == z.q.sessionTransitions && playback.trackEnds == z.q.sessionTrackEnds &&
                                 z.q.entryId == z.q.sessionEntry && playback.file == z.q.sessionFile;
            if (!current || playback.state == audio::PlayState::Stopped) {
                // A gapless transition or natural end heard the rest of the track; a failure keeps what was observed.
                if (playback.transitions != z.q.sessionTransitions ||
                    (playback.trackEnds != z.q.sessionTrackEnds && !playback.error))
                    z.q.session->finish();
                z.q.session->persist(*library_);
                z.q.session.reset();
            } else if (playback.state == audio::PlayState::Playing) {
                // Rendering can move into a queued file before the engine publishes the transition; a jump back
                // to its start belongs to the next file, so this session waits for the transition instead.
                const auto last = z.q.session->position();
                const bool nextFile = playback.queuedFiles > 0 && last && playback.position < *last &&
                                      playback.position <= reachable(z.q.sessionRate);
                if (!nextFile) z.q.session->observe(playback.position, reachable(z.q.sessionRate));
            } else {
                z.q.session->interrupt();
            }
        }
        if (!z.q.session && z.q.entryId && playback.state != audio::PlayState::Stopped && playback.source &&
            playback.source->sampleRate) {
            const auto binding = z.q.libraryEntries.find(z.q.entryId);
            if (binding != z.q.libraryEntries.end() && binding->second.file &&
                playback.file == toWide(entry(z, z.q.entryId).source.pathUtf8())) {
                z.q.session.emplace(LibraryTrackKey{binding->second.file}, playback.source->sampleRate, playback.total);
                z.q.session->setZone(z.state.name);
                z.q.sessionEntry = z.q.entryId;
                z.q.sessionFile = playback.file;
                z.q.sessionTransitions = playback.transitions;
                z.q.sessionTrackEnds = playback.trackEnds;
                z.q.sessionRate = playback.source->sampleRate;
                z.q.sessionPersisted = now;
                z.q.sessionCountPersisted = false;
                // A track that started since the previous pass is credited from its first frame.
                if (playback.state == audio::PlayState::Playing) {
                    z.q.session->observe(0, 0);
                    z.q.session->observe(playback.position, reachable(z.q.sessionRate));
                }
            }
        }
        z.q.sessionSeen = now;
        if (!z.q.session) return;
        if (now - z.q.sessionPersisted >= std::chrono::seconds(30) || (z.q.session->counted() && !z.q.sessionCountPersisted)) {
            z.q.session->persist(*library_);
            z.q.sessionPersisted = now;
            z.q.sessionCountPersisted = z.q.session->counted();
        }
    }

    // Keeps the entries that follow queued in the engine while a stream is open, so they play gaplessly. The engine
    // takes one more as soon as its decoder moves on to the previous one, seconds before that one is heard.
    void ensureNextQueued(Zone& z) {
        const auto playback = z.engine->snapshot();
        if (playback.transitions != z.seenTransitions) return; // committed on the next pass
        if (playback.queuedFiles == 0) z.chain.clear();        // a restarted stream forgets queued files
        if (!playback.output || !z.q.entryId || !z.q.track) return;
        // Opening the next file while mpg123 scans this one would wait behind the scan's reads, and this thread with it,
        // which holds back every command: it waits for the scan, or for the last seconds of the track.
        if (playback.decoder == audio::DecoderKind::Mpg123 && playback.source && !playback.source->totalFrames) {
            constexpr uint64_t kLastSeconds = 20;
            const uint64_t left = playback.total > playback.position ? playback.total - playback.position : 0;
            if (left > kLastSeconds * playback.source->sampleRate) return;
        }
        // Predicting copies the policy, which is costly for a large queue; it is redone only when its inputs changed.
        const QueueCheck check{z.q.policy.revision(), z.q.entryId, {z.chain.begin(), z.chain.end()}, playback.nextSlotFree};
        if (check == z.checked) return;
        z.checked = check;
        auto copy = z.q.policy;
        for (size_t i = 0; i < z.chain.size(); ++i) {
            const auto predicted = copy.next(true);
            if (predicted && predicted->entryId == z.chain[i]) continue;
            // The order changed: withdraw a file still waiting in the slot; one already decoding plays regardless.
            if (i + 1 == z.chain.size() && !playback.nextSlotFree && z.engine->clearNext()) {
                z.chain.pop_back();
                return;
            }
            copy.select(z.chain[i]);
        }
        if (!playback.nextSlotFree || z.chain.size() >= 2) return;
        const auto upcoming = copy.next(true); // the copy is the prediction's own, so upcoming() would copy it again
        if (!upcoming) return;
        const QueueAttempt attempt{z.q.entryId, upcoming->entryId, z.q.policy.revision(), z.chain.size()};
        if (attempt == z.attempted) return;
        z.attempted = attempt;
        const auto& source = playable(z, upcoming->entryId);
        if (source.decoder() != z.q.track->decoder()) return;
        const auto from = z.chain.empty() ? z.q.entryId : z.chain.back();
        shareLibraryLength(z, upcoming->entryId);
        const std::filesystem::path file(toWide(source.pathUtf8()));
        if (z.engine->queueNext(file, source.decoder(), gainFor(z, upcoming->entryId), crossfadeMs(z, from, upcoming->entryId))) {
            z.chain.push_back(upcoming->entryId);
            std::lock_guard lock(readsMutex_);
            z.queuedRead = file;
        }
    }

    // What playback reads, for the library's background work (docs/Plan.md §7.7): any thread may ask. Ahead while
    // every playing zone is.
    PlaybackReads playbackReads() const {
        PlaybackReads reads;
        bool ahead = true;
        std::shared_lock zones(zonesMutex_);
        for (const auto& zone : zones_) {
            const auto playback = zone->engine->snapshot();
            if (playback.state != audio::PlayState::Playing) continue;
            reads.files.emplace_back(playback.file);
            if (playback.queuedFiles) {
                std::lock_guard lock(readsMutex_);
                reads.files.push_back(zone->queuedRead);
            }
            ahead = ahead && playback.ahead;
        }
        reads.ahead = !reads.files.empty() && ahead;
        return reads;
    }

    // While any zone plays, to any output and whoever started it, Windows does not sleep; the display still may
    // (MAC-R27). The state is this thread's, which lives as long as the Server.
    void keepAwakeWhilePlaying() {
        const bool playing = std::ranges::any_of(zones_, [](const auto& zone) {
            const auto playback = zone->engine->snapshot();
            return playback.state == audio::PlayState::Playing || playback.bufferingSeconds;
        });
        if (playing == keptAwake_) return;
        keptAwake_ = playing;
        SetThreadExecutionState(playing ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
        log::info(playing ? "power: Windows kept from sleeping while zones play" : "power: Windows may sleep again");
    }

    bool anyZonePlaying() const {
        std::shared_lock zones(zonesMutex_);
        return std::ranges::any_of(zones_,
                                   [](const auto& zone) { return zone->engine->snapshot().state == audio::PlayState::Playing; });
    }

    // The crossfade from one entry into the next (docs/Plan.md §5.4): none while an album plays in order, the next
    // entry from the same album with shuffle off.
    uint32_t crossfadeMs(const Zone& z, uint64_t from, uint64_t to) const {
        if (!z.settings.crossfade.enabled) return 0;
        const auto album = [&z](uint64_t id) {
            const auto at = std::ranges::find(z.q.entries, id, &QueueEntry::id);
            return at == z.q.entries.end() ? int64_t{0} : at->albumId;
        };
        if (!z.q.policy.shuffle() && album(from) != 0 && album(from) == album(to)) return 0;
        return uint32_t(std::lround(z.settings.crossfade.seconds * 1000.0));
    }

    // The length the library knows for the entry's MP3, exact or estimated, which the MP3 decoders show at once and place
    // early seeks by.
    void shareLibraryLength(Zone& z, uint64_t entryId) {
        const auto at = std::ranges::find(z.q.entries, entryId, &QueueEntry::id);
        if (at == z.q.entries.end() || !at->file || !libraryReader_) return;
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
    double gainFor(Zone& z, uint64_t entryId) {
        if (z.settings.replayGain.mode == ReplayGainMode::Off) return 0.0;
        const auto at = std::ranges::find(z.q.entries, entryId, &QueueEntry::id);
        TrackLoudness loudness;
        if (at != z.q.entries.end() && at->file && libraryReader_) {
            try {
                loudness = trackLoudness(*libraryReader_, at->file);
            } catch (const std::exception& e) {
                log::warn("ReplayGain: %s", e.what());
            }
        }
        bool album = false;
        if (at != z.q.entries.end() && at->albumId != 0 && !z.q.policy.shuffle()) {
            album = (at != z.q.entries.begin() && std::prev(at)->albumId == at->albumId) ||
                    (std::next(at) != z.q.entries.end() && std::next(at)->albumId == at->albumId);
        }
        return replayGainDb(z.settings.replayGain, loudness, album);
    }

    // What the audio analysis reads from another thread: the ReplayGain and waveform settings, the decoders, the files of
    // the current and next queue entries, which it analyzes first, and the playing track and the next for their waveforms.
    // Every zone's, the selected zone's first.
    void shareLoudnessInputs() {
        std::vector<int64_t> files;
        std::vector<LoudnessAnalyzer::PlayingTrack> playing;
        std::vector<const Zone*> order{&selected()};
        for (const auto& zone : zones_)
            if (zone.get() != order.front()) order.push_back(zone.get());
        for (const Zone* zone : order) {
            const auto& entries = zone->q.entries;
            auto at = std::ranges::find(entries, zone->q.entryId, &QueueEntry::id);
            if (at == entries.end()) at = entries.begin();
            size_t taken = 0, shown = 0;
            for (; at != entries.end() && taken < 20; ++at, ++taken) {
                if (at->file) files.push_back(at->file);
                if (shown++ < 2) playing.push_back({at->file, std::filesystem::path(toWide(at->source.pathUtf8()))});
            }
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
    void advanceAfterTrackEnd(Zone& z) {
        const auto playback = z.engine->snapshot();
        if (playback.trackEnds == z.seenTrackEnds) return;
        z.seenTrackEnds = playback.trackEnds;
        if (!z.q.entryId) return;
        if (playback.error) {
            if (!trackLocal(*playback.error)) return;
            z.q.policy.markFailed(z.q.entryId);
            z.queueChanged = true;
            ++z.q.failuresInARow;
        } else {
            z.q.failuresInARow = 0;
        }
        // Autoplay gives up on tracks that keep failing, as when the output takes none of the library's formats.
        const bool autoplay = autoplayAllowed(z) && z.q.failuresInARow < kAutoplayFailuresInARow;
        if (const auto next = z.q.policy.next(true))
            startEntry(z, *next, true, true);
        else if (const auto picked = autoplay && appendAutoplay(z) ? z.q.policy.next(true) : std::nullopt)
            startEntry(z, *picked, true, true);
    }

    // Autoplay (docs/Plan.md §8.1): once the last entry plays, a random library track follows it, so it preloads
    // for gapless playback. Each entry gets one try; a repeat mode never ends the queue.
    bool autoplayAllowed(const Zone& z) const {
        return z.settings.autoplay && library_ && z.q.policy.repeat() == RepeatMode::Off;
    }

    void extendWithAutoplay(Zone& z) {
        if (!autoplayAllowed(z) || !z.q.entryId || z.q.autoplayTried == z.q.entryId) return;
        if (z.engine->snapshot().state != audio::PlayState::Playing) return;
        z.q.autoplayTried = z.q.entryId;
        if (z.q.policy.upcoming()) return;
        try {
            appendAutoplay(z);
        } catch (const std::exception& e) {
            log::warn("autoplay: %s", e.what());
        }
    }

    // A random available library track not played in the last 7 days and not in the queue; when every track was
    // played that recently, the one played longest ago.
    std::optional<TrackSource> pickAutoplayTrack(Zone& z) {
        if (!libraryReader_) libraryReader_.emplace(library_->openReader());
        nlohmann::json queued = nlohmann::json::array();
        for (const auto& [id, binding] : z.q.libraryEntries)
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

    bool appendAutoplay(Zone& z) {
        const auto source = pickAutoplayTrack(z);
        if (!source) return false;
        auto entries = buildQueueEntries(z, {*source});
        entries.front().automatic = true;
        z.q.policy.append({entries.front().id}, false);
        z.q.entries.push_back(std::move(entries.front()));
        z.q.libraryVersion = 0;
        refreshLibraryAvailability(z);
        z.queueChanged = true;
        // Played automatic entries beyond the latest 100 go, oldest first; the user's entries stay.
        std::vector<uint64_t> played;
        for (const auto& entry : z.q.entries) {
            if (entry.id == z.q.entryId) break;
            if (entry.automatic) played.push_back(entry.id);
        }
        if (played.size() > kAutoplayKept) {
            played.resize(played.size() - kAutoplayKept);
            removeQueue(z, played);
        }
        return true;
    }

    QueueSnapshot queueSnapshot(const Zone& z) const {
        QueueSnapshot queue;
        queue.revision = z.q.policy.revision();
        queue.marks = marks_;
        queue.entries = z.q.entries;
        if (const auto upcoming = z.q.policy.upcoming()) queue.upcomingEntryId = upcoming->entryId;
        queue.shuffle = z.q.policy.shuffle();
        queue.repeat = z.q.policy.repeat();
        return queue;
    }

    // Builds what the zone publishes; true when it changed.
    bool refreshZoneState(Zone& z) {
        auto p = playback(z.engine->snapshot());
        if (z.backend == Backend::AirPlay) {
            std::scoped_lock lock(z.airplay->mutex);
            p.receivers = z.airplay->states;
            p.receiverAsking = askingName(*z.airplay);
        }
        p.receiverLeft = z.receiverLeft;
        if (receiverVolumeWanted(z)) {
            const auto level = receiverLevel(z);
            p.playerVolume = level.level;
            p.playerMuted = level.muted;
        } else if (const auto level = z.volume ? z.volume->current() : std::nullopt) {
            p.playerVolume = level->level;
            p.playerVolumeDb = level->db;
            p.playerMuted = level->muted;
        }
        if (z.q.restored && p.state == PlaybackState::Stopped && z.q.track) {
            p.state = PlaybackState::Paused;
            p.pathUtf8 = z.q.track->pathUtf8();
            p.position = z.q.restored->position;
            p.total = z.q.restored->total;
            p.source = z.q.restored->source;
            p.format = z.q.restored->format;
        }
        const auto& was = z.state.playback;
        if (p.state == PlaybackState::Playing && was.state != PlaybackState::Playing) z.state.played = ++playSerial_;
        bool changed = z.queueChanged || p.revision != was.revision || p.position != was.position ||
                       p.underruns != was.underruns || p.playerVolume != was.playerVolume ||
                       p.playerVolumeDb != was.playerVolumeDb || p.playerMuted != was.playerMuted ||
                       p.receivers != was.receivers || p.receiverLeft != was.receiverLeft ||
                       p.receiverAsking != was.receiverAsking || z.state.queueEntryId != z.q.entryId;
        z.state.playback = std::move(p);
        // Rebuilt only when it changed: a large queue is not copied on every position update.
        if (z.queueChanged || z.state.queue.revision != z.q.policy.revision() || z.publishedEntry != z.q.entryId) {
            z.state.queue = queueSnapshot(z);
            z.publishedEntry = z.q.entryId;
            changed = true;
        }
        z.state.queueEntryId = z.q.entryId;
        z.state.track = z.q.track;
        const Zone* holder = holderFor(z);
        const std::string inUseBy = holder ? holder->id : std::string();
        changed = changed || inUseBy != z.state.inUseBy;
        z.state.inUseBy = inUseBy;
        z.queueChanged = false;
        return changed;
    }

    // Every zone's state into state_, the selected zone's also as the window's.
    void collectZones() {
        state_.zones.clear();
        state_.zones.reserve(zones_.size());
        for (const auto& zone : zones_) state_.zones.push_back(zone->state);
        const Zone& shown = selected();
        state_.selectedZone = shown.id;
        state_.queueEntryId = shown.state.queueEntryId;
        state_.track = shown.state.track;
        state_.queue = shown.state.queue;
        state_.playback = shown.state.playback;
    }

    void publish(bool completed) {
        // Settings share them when applied; here only a change to a queue or its current entry does.
        std::vector<std::pair<uint64_t, uint64_t>> shared;
        bool queues = false;
        for (const auto& zone : zones_) {
            shared.emplace_back(zone->q.policy.revision(), zone->q.entryId);
            queues = queues || zone->queueChanged;
        }
        if (queues || shared != loudnessShared_) {
            loudnessShared_ = std::move(shared);
            shareLoudnessInputs();
        }
        bool changed = completed || std::exchange(zonesChanged_, false) ||
                       state_.devices.revision != published_.devices.revision || state_.finished != published_.finished;
        for (auto& zone : zones_) changed = refreshZoneState(*zone) || changed;
        std::optional<Caller> rebuilt;
        {
            std::lock_guard lock(mutex_);
            rebuilt = std::exchange(rebuiltBy_, std::nullopt);
        }
        if (rebuilt) {
            state_.notice = {++changeSerial_, "libraryRebuilt", rebuilt->device, rebuilt->name, {}};
            changed = true;
        }
        if (!changed) return;
        collectZones();
        state_.settings = settingsDto(settings_);
        Event event;
        std::vector<std::function<void(const Event&)>> listeners;
        {
            std::lock_guard lock(mutex_);
            if (state_.command.generation != issued_.generation || state_.command.queueRevision != issued_.queueRevision) return;
            state_.revision = published_.revision + 1;
            published_ = state_;
            // A receiver CamelPlayer cannot play is not offered; the Server still knows it, to take it out of use. One that
            // asks for a password or a code is, to be paired.
            std::erase_if(published_.devices.airplay, [](const DeviceInfo& d) {
                return !d.unsupported.empty() && d.unsupported != "password" && d.unsupported != "code";
            });
            event = {completed ? EventKind::CommandCompleted : EventKind::StateChanged, published_};
            listeners = audience();
        }
        for (const auto& listener : listeners) tell(listener, event);
        if (discord_) discord_->observe(event.snapshot);
    }

    static void tell(const std::function<void(const Event&)>& listener, const Event& event) {
        if (!listener) return;
        try {
            listener(event);
        } catch (...) {
            log::warn("Server listener failed");
        }
    }

    // Said once a request has waited a second: most receivers answer at once, a Mac once its user does.
    static std::string askingName(const AirPlayGroup& group) {
        if (!group.asking || std::chrono::steady_clock::now() - group.asking->second < kAskingShownAfter) return {};
        const auto named = std::ranges::find(group.states, group.asking->first, &ReceiverState::id);
        return named == group.states.end() ? group.asking->first : named->name;
    }

    // An output opening waits on a receiver that asks its user, and this thread on the output: meanwhile only that
    // changes in what was published.
    void waitForEngine(Zone& z) {
        z.engine->waitIdle(std::chrono::milliseconds(50), [this, &z] {
            if (z.backend != Backend::AirPlay) return;
            std::string asking;
            {
                std::scoped_lock lock(z.airplay->mutex);
                asking = askingName(*z.airplay);
            }
            Event event;
            std::vector<std::function<void(const Event&)>> listeners;
            {
                std::lock_guard lock(mutex_);
                const auto shown = std::ranges::find(published_.zones, z.id, &ZoneSnapshot::id);
                if (shown == published_.zones.end() || shown->playback.receiverAsking == asking) return;
                shown->playback.receiverAsking = asking;
                if (published_.selectedZone == z.id) published_.playback.receiverAsking = asking;
                ++published_.revision;
                event = {EventKind::StateChanged, published_};
                listeners = audience();
            }
            for (const auto& listener : listeners) tell(listener, event);
        });
    }

    void refreshDevices() {
        DeviceCatalog catalog;
        const LocalDevices local = localDevices_ ? localDevices_()
                                                 : LocalDevices{audio::listWasapiDevices(), audio::listAsioDrivers(),
                                                                audio::listKsPins(), audio::defaultWasapiDevice()};
        catalog.wasapi = devices(local.wasapi);
        catalog.asio = devices(local.asio);
        catalog.ks = devices(local.ks);
        catalog.defaultWasapi = local.defaultWasapi;
        findNetworkPlayers(catalog);
        findAirPlayReceivers(catalog);
        findHQPlayers(catalog);
        catalog.revision = state_.devices.revision + 1;
        state_.devices = std::move(catalog);
        const auto outputs = settings_.outputs;
        const auto followed = settings_.followedDefault;
        const auto receiver = settings_.backend == Backend::AirPlay ? settings_.airplay.deviceId : std::string();
        followMoves(syncOutputs(settings_, state_.devices));
        // Speakers paired while one of them played: the pair is in use now and plays on once it can.
        if (!receiver.empty() && settings_.airplay.deviceId != receiver && selected_)
            selected_->pairedAt = std::chrono::steady_clock::now();
        if (settings_.outputs == outputs && settings_.followedDefault == followed) return;
        syncZones();
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
        for (const auto& player : finder_->players()) {
            const auto& d = player.description;
            DeviceInfo device{d.udn, d.name};
            device.model = d.manufacturer.empty() ? d.model : d.model.empty() ? d.manufacturer : d.manufacturer + " " + d.model;
            device.mac = player.mac;
            catalog.network.push_back(std::move(device));
        }
    }

    // Those that answer a search, and those added by address that something listens at.
    void findHQPlayers(DeviceCatalog& catalog) {
        std::vector<std::string> added;
        for (const auto& saved : settings_.outputs)
            if (saved.backend == Backend::HQPlayer && !saved.id.empty()) added.push_back(saved.id);
        std::scoped_lock lock(finderMutex_);
        if (hqplayerFinder_) {
            hqplayerFinder_->search();
        } else {
            hqplayerFinder_ = std::make_unique<hqplayer::Finder>(hqplayerFinderOptions_, [this] {
                devicesChangedAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
            });
        }
        hqplayerFinder_->watch(std::move(added));
        for (const auto& found : hqplayerFinder_->found()) {
            std::string name = found.name;
            if (name.empty())
                if (const auto saved = std::ranges::find_if(
                        settings_.outputs,
                        [&](const SavedOutput& o) { return o.backend == Backend::HQPlayer && o.id == found.address; });
                    saved != settings_.outputs.end())
                    name = saved->name;
            catalog.hqplayer.push_back({found.address, name.empty() ? found.address : name});
        }
    }

    void findAirPlayReceivers(DeviceCatalog& catalog) {
        if (airplayFinder_) {
            airplayFinder_->search();
        } else {
            std::scoped_lock lock(finderMutex_);
            auto options = airplayFinderOptions_;
            options.pairings = airplayPairings_;
            airplayFinder_ = std::make_unique<AirPlayFinder>(std::move(options), [this] {
                devicesChangedAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
            });
        }
        for (const auto& receiver : airplayFinder_->receivers()) {
            const std::string why = airplay::supportId(receiver.support);
            if (auto& logged = airplaySupportLogged_[receiver.id]; logged != why) {
                if (!why.empty())
                    log::info("AirPlay receiver %s (%s) cannot be played now: %s", receiver.name.c_str(), receiver.id.c_str(),
                              why.c_str());
                logged = why;
            }
            catalog.airplay.push_back({receiver.id, receiver.name, why, receiver.speakers});
        }
    }

    // A receiver deleted in Settings is no longer paired: added again, it asks for its password or code again.
    void forgetDeletedReceivers(const std::vector<SavedOutput>& before) {
        for (const auto& output : before) {
            const auto same = [&](const SavedOutput& o) { return o.backend == output.backend && o.id == output.id; };
            if (output.backend != Backend::AirPlay || std::ranges::any_of(settings_.outputs, same) ||
                !airplayPairings_->find(output.id))
                continue;
            airplayPairings_->forget(output.id);
            log::info("AirPlay receiver %s deleted: its pairing is forgotten", output.id.c_str());
            if (airplayFinder_) airplayFinder_->check(output.id);
        }
    }

    std::optional<airplay::Receiver> airplayReceiver(const std::string& id) {
        std::scoped_lock lock(finderMutex_);
        if (!airplayFinder_) return std::nullopt;
        const auto found = airplayFinder_->receivers();
        const auto receiver = std::ranges::find(found, id, &airplay::Receiver::id);
        return receiver == found.end() ? std::nullopt : std::optional(*receiver);
    }

    void showAirPlayCode(const std::string& id) {
        const auto receiver = airplayReceiver(id);
        if (!receiver) throw audio::Error(audio::ErrorCode::DeviceNotFound, "the AirPlay receiver is not connected");
        try {
            auto client = std::make_unique<airplay::RtspClient>(receiver->address, receiver->port, kPairingTimeoutMs);
            if (!client->showPin()) throw audio::Error(audio::ErrorCode::DriverError, "the receiver shows no code");
            codeShown_ = {id, std::move(client)};
        } catch (const audio::Error&) {
            throw;
        } catch (const std::exception& e) {
            throw audio::Error(audio::ErrorCode::DeviceNotFound, e.what());
        }
    }

    // Pairs with what the user typed: a password is tried as transient pairing's code, then as pair-setup's; a code
    // shown on the receiver as pair-setup's, on the connection that asked for it while it is open.
    void pairAirPlay(const std::string& id, const std::string& code) {
        const auto receiver = airplayReceiver(id);
        if (!receiver) throw audio::Error(audio::ErrorCode::DeviceNotFound, "the AirPlay receiver is not connected");
        AirPlayCredentials credentials;
        auto outcome = airplay::PairOutcome::Refused;
        try {
            if (receiver->support == airplay::Support::Password) {
                airplay::RtspClient client(receiver->address, receiver->port, kPairingTimeoutMs);
                outcome = client.pairTransiently(code);
                if (outcome == airplay::PairOutcome::Paired) credentials.password = code;
            }
            if (receiver->support != airplay::Support::Password || outcome == airplay::PairOutcome::WrongCode) {
                auto client = codeShown_.first == id && codeShown_.second && !codeShown_.second->closed()
                                  ? std::move(codeShown_.second)
                                  : std::make_unique<airplay::RtspClient>(receiver->address, receiver->port, kPairingTimeoutMs);
                codeShown_ = {};
                airplay::PairedReceiver keys;
                outcome = client->pairSetup(code, airplayPairings_->identity(), keys);
                if (outcome == airplay::PairOutcome::Paired) credentials.keys = keys;
            }
        } catch (const std::exception& e) {
            throw audio::Error(audio::ErrorCode::DeviceNotFound, e.what());
        }
        if (outcome == airplay::PairOutcome::WrongCode)
            throw audio::Error(audio::ErrorCode::AccessDenied, "the password or code is not the receiver's");
        if (outcome != airplay::PairOutcome::Paired)
            throw audio::Error(audio::ErrorCode::DriverError, "the receiver did not pair");
        airplayPairings_->save(id, credentials);
        log::info("AirPlay receiver %s (%s) paired %s", receiver->name.c_str(), id.c_str(),
                  credentials.keys ? "with keys" : "with its password");
        {
            std::scoped_lock lock(finderMutex_);
            airplayFinder_->paired(id);
        }
        // The catalog this command publishes lists it as playable, so the user can turn it on at once.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = airplayReceiver(id);
            if (now && now->support == airplay::Support::Supported) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void checkEnabledReceivers(const std::vector<SavedOutput>& before) {
        if (!airplayFinder_) return;
        for (const auto& output : settings_.outputs) {
            const auto same = [&](const SavedOutput& o) { return o.backend == output.backend && o.id == output.id && o.enabled; };
            if (output.backend == Backend::AirPlay && output.enabled && std::ranges::none_of(before, same))
                airplayFinder_->check(output.id);
        }
    }

    // The receivers of a zone's group as its output asks for them, from any thread (AirPlayOutput.h).
    std::unique_ptr<audio::IOutput> airplayOutput(const Settings& settings, const std::shared_ptr<AirPlayGroup>& group) {
        waitForChecks(*group);
        // A group refused, by its access setting or by its answer, is asked again at the Play its message asks for: the
        // user may just have let this computer in, which the finder learns only when it asks.
        std::vector<std::string> refused;
        {
            std::scoped_lock lock(group->mutex);
            if (!group->refusal.empty() &&
                std::ranges::none_of(group->members, [](const AirPlayMember& m) { return m.receiver.has_value(); }))
                for (const auto& member : group->members) refused.push_back(member.id);
        }
        if (!refused.empty()) {
            {
                std::scoped_lock lock(finderMutex_);
                if (airplayFinder_) {
                    airplayFinder_->search();
                    for (const auto& id : refused) airplayFinder_->check(id);
                }
            }
            // The finder's answer is taken here: the Server thread, which follows the group, may be waiting for this output.
            const auto deadline = std::chrono::steady_clock::now() + kAskAgainAtPlay;
            while (std::chrono::steady_clock::now() < deadline) {
                std::vector<airplay::Receiver> found;
                {
                    std::scoped_lock lock(finderMutex_);
                    if (airplayFinder_) found = airplayFinder_->receivers();
                }
                {
                    std::scoped_lock lock(group->mutex);
                    for (auto& member : group->members) {
                        const auto receiver = std::ranges::find(found, member.id, &airplay::Receiver::id);
                        if (receiver == found.end()) continue;
                        member.support = receiver->support;
                        if (receiver->support == airplay::Support::Supported) member.receiver = *receiver;
                    }
                    if (std::ranges::any_of(group->members, [](const AirPlayMember& m) { return m.receiver.has_value(); })) {
                        group->refusal.clear();
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        {
            std::scoped_lock lock(group->mutex);
            if (std::ranges::none_of(group->members, [](const AirPlayMember& m) { return m.receiver.has_value(); })) {
                if (!group->refusal.empty()) throw audio::Error(audio::ErrorCode::AccessDenied, group->refusal);
                throw audio::Error(audio::ErrorCode::DeviceNotFound, "the AirPlay receiver is not connected");
            }
        }
        AirPlayOutputOptions options;
        options.pairings = airplayPairings_;
        options.details = [this](const std::wstring& file) { return trackDetails(file); };
        options.lookAgain = [this] {
            std::scoped_lock lock(finderMutex_);
            if (airplayFinder_) airplayFinder_->search();
        };
        options.members = [group] {
            std::scoped_lock lock(group->mutex);
            return group->members;
        };
        options.memberLeft = [group](const std::string& id, const audio::Error& why) {
            std::scoped_lock lock(group->mutex);
            group->left.emplace_back(id, why);
        };
        options.commanded = [this, group, zone = zoneId(Backend::AirPlay, settings.airplay.deviceId)](
                                const std::string& id, const airplay::ReceiverEvent& event) {
            followReceiver(zone, *group, id, event);
        };
        options.askingUser = [group](const std::string& id, bool asking) {
            std::scoped_lock lock(group->mutex);
            if (asking)
                group->asking.emplace(id, std::chrono::steady_clock::now());
            else if (group->asking && group->asking->first == id) {
                const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - group->asking->second;
                if (waited >= kAskingShownAfter)
                    log::info("AirPlay receiver %s answered after %.1f s", id.c_str(), waited.count());
                group->asking.reset();
            }
        };
        return createAirPlayGroupOutput(std::move(options));
    }

    // A receiver just chosen or turned on is asked for a session by the finder. The owner's DMP-A6 closed the stream's
    // connection while it served that check, and a Play right after choosing it failed with "not connected", so the stream
    // waits for the group's checks; a Mac's waits for its user's answer, up to 15 s.
    void waitForChecks(AirPlayGroup& group) {
        std::vector<std::string> ids;
        {
            std::scoped_lock lock(group.mutex);
            for (const auto& member : group.members) ids.push_back(member.id);
        }
        const auto deadline = std::chrono::steady_clock::now() + kChecksWait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::scoped_lock lock(finderMutex_);
                if (!airplayFinder_ ||
                    std::ranges::none_of(ids, [&](const std::string& id) { return airplayFinder_->checking(id); }))
                    return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // An AirPlay 2 receiver's own buttons act as the same buttons in CamelPlayer, in its zone; its volume is taken on the
    // Server's thread.
    void followReceiver(const std::string& zone, AirPlayGroup& group, const std::string& id,
                        const airplay::ReceiverEvent& event) {
        using airplay::ReceiverCommand;
        if (event.command == ReceiverCommand::Volume) {
            std::scoped_lock lock(group.mutex);
            group.volumes.emplace_back(id, event);
            return;
        }
        Command command;
        command.kind = event.command == ReceiverCommand::TogglePause ? CommandKind::TogglePause
                       : event.command == ReceiverCommand::Next      ? CommandKind::Next
                       : event.command == ReceiverCommand::Previous  ? CommandKind::Previous
                                                                     : CommandKind::Pause;
        command.zone = zone;
        try {
            enqueue(std::move(command));
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", id.c_str(), e.what()); // the Server is closing
        }
    }

    static bool receiverVolumeWanted(const Zone& z) {
        return z.backend == Backend::AirPlay && z.settings.volume.mode == VolumeMode::Player;
    }

    AirPlayLevel levelOf(const std::string& id) const {
        const auto found = settings_.airplayLevels.find(id);
        return found == settings_.airplayLevels.end() ? AirPlayLevel{} : found->second;
    }

    // A zone's AirPlay receivers, the one it was formed with first, without one that another zone is or that a group before
    // it plays through: the group in use's first, then the others' in the output menu's order.
    std::vector<std::string> airplayGroup(const Zone& z) const {
        if (z.backend != Backend::AirPlay) return {};
        const auto order = [this](const Zone* zone) {
            if (zone == selected_) return std::ptrdiff_t(-1);
            return std::ranges::find_if(zones_, [&](const auto& held) { return held.get() == zone; }) - zones_.begin();
        };
        std::vector<std::string> group{z.output};
        for (const auto& id : z.settings.airplayGroup) {
            if (std::ranges::find(group, id) != group.end()) continue;
            const bool elsewhere = std::ranges::any_of(zones_, [&](const auto& other) {
                if (other.get() == &z || other->backend != Backend::AirPlay) return false;
                return other->output == id ||
                       (order(other.get()) < order(&z) &&
                        std::ranges::find(other->settings.airplayGroup, id) != other->settings.airplayGroup.end());
            });
            if (!elsewhere) group.push_back(id);
        }
        return group;
    }

    // A group's level is its loudest receiver's, muted when every receiver is.
    AirPlayLevel receiverLevel(const Zone& z) const {
        const auto group = airplayGroup(z);
        if (group.size() < 2) return levelOf(z.output);
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
    void changeReceiverLevel(Zone& z, const std::function<void(AirPlayLevel&)>& change) {
        const AirPlayLevel was = receiverLevel(z);
        AirPlayLevel wanted = was;
        change(wanted);
        const auto now = std::chrono::steady_clock::now();
        if (!z.groupBaseline || now - z.groupBaseline->at > kGroupDragSettles)
            z.groupBaseline = GroupBaseline{was.level, {}, now};
        z.groupBaseline->at = now;
        for (const auto& id : airplayGroup(z)) {
            AirPlayLevel& level = settings_.airplayLevels.try_emplace(id).first->second;
            const uint32_t base = z.groupBaseline->levels.try_emplace(id, level.level).first->second;
            if (wanted.level != was.level)
                level.level = z.groupBaseline->master
                                  ? std::min(100u, uint32_t(std::lround(double(base) * wanted.level / z.groupBaseline->master)))
                                  : wanted.level;
            if (wanted.muted != was.muted) level.muted = wanted.muted;
        }
        persistSoon();
    }

    std::string receiverName(const std::string& id, const std::vector<airplay::Receiver>& found) const {
        const auto saved = std::ranges::find_if(
            settings_.outputs, [&](const SavedOutput& o) { return o.backend == Backend::AirPlay && o.id == id; });
        if (saved != settings_.outputs.end() && !saved->alias.empty()) return saved->alias;
        const auto receiver = std::ranges::find(found, id, &airplay::Receiver::id);
        if (receiver != found.end()) return receiver->name;
        return saved != settings_.outputs.end() && !saved->name.empty() ? saved->name : id;
    }

    // The stream to the speaker ends as it goes into the pair, before or after the pair is found: within a while of the
    // pairing, a stream that ended so plays on as the pair once the pair can take it.
    void resumeAsPairOnceFound(Zone& z) {
        if (!z.pairedAt) return;
        if (std::chrono::steady_clock::now() - *z.pairedAt > kResumeAsPairWithin) {
            z.pairedAt.reset();
            return;
        }
        const auto playback = z.engine->snapshot();
        if (playback.state != audio::PlayState::Paused ||
            (playback.error != audio::ErrorCode::DeviceChanged && playback.error != audio::ErrorCode::DeviceInUse))
            return;
        {
            std::scoped_lock lock(z.airplay->mutex);
            if (std::ranges::none_of(z.airplay->members, [](const AirPlayMember& m) { return m.receiver.has_value(); })) return;
        }
        z.pairedAt.reset();
        resumePlayback(z);
    }

    // What each AirPlay zone's output reads from any thread: its receivers as found, their channels and levels. A
    // receiver that left while the others play on leaves the group, which says why.
    void followAirPlayGroup() {
        std::vector<airplay::Receiver> found;
        {
            std::scoped_lock lock(finderMutex_);
            if (airplayFinder_) found = airplayFinder_->receivers();
        }
        bool grouping = false;
        for (auto& zone : zones_) {
            Zone& z = *zone;
            if (z.backend != Backend::AirPlay) continue;
            std::vector<std::pair<std::string, audio::Error>> left;
            std::vector<std::pair<std::string, airplay::ReceiverEvent>> volumes;
            {
                std::scoped_lock lock(z.airplay->mutex);
                left.swap(z.airplay->left);
                volumes.swap(z.airplay->volumes);
            }
            const auto group = airplayGroup(z);
            // A receiver's own volume becomes the level CamelPlayer shows and keeps for it.
            for (const auto& [id, event] : volumes) {
                if (!receiverVolumeWanted(z) || std::ranges::find(group, id) == group.end()) continue;
                settings_.airplayLevels[id] = {uint32_t(std::lround(event.volume * 100)), event.muted};
                z.groupBaseline.reset();
                persistSoon();
            }
            for (const auto& [id, why] : left) {
                if (std::ranges::find(group, id) == group.end() || group.size() < 2) continue;
                z.receiverLeft = ReceiverLeft{z.receiverLeft ? z.receiverLeft->serial + 1 : 1, receiverName(id, found),
                                              why.code(), why.what()};
                if (&z == selected_) {
                    setInAirPlayGroup(settings_, id, false, state_.devices);
                } else if (id == z.output) {
                    // The zone goes on under the first receiver left (MZ-R08).
                    if (const auto next = passAirPlayGroup(settings_, z.output); !next.empty()) {
                        std::unique_lock zones(zonesMutex_);
                        rekey(z, Backend::AirPlay, next);
                    }
                } else {
                    leaveAirPlayGroup(settings_, z.output, id);
                }
                grouping = true;
            }
            std::vector<AirPlayMember> members;
            std::vector<ReceiverState> states;
            std::string refusal; // the first member's that keeps this computer out
            const bool player = receiverVolumeWanted(z);
            for (const auto& id : airplayGroup(z)) {
                AirPlayMember member{id};
                const auto receiver = std::ranges::find(found, id, &airplay::Receiver::id);
                if (receiver != found.end()) {
                    member.support = receiver->support;
                    if (receiver->support == airplay::Support::Supported)
                        member.receiver = *receiver;
                    else if (refusal.empty())
                        refusal = receiver->mac && receiver->support == airplay::Support::Refused
                                      ? std::string(airplay::kMacRefused)
                                      : "the receiver refuses this connection";
                }
                member.channels = airplayChannels(settings_, id);
                const AirPlayLevel level = levelOf(id);
                if (player) member.volumeDb = airplay::volumeDb(level.level, level.muted);
                states.push_back({id, receiverName(id, found), level.level, level.muted, member.channels});
                members.push_back(std::move(member));
            }
            std::scoped_lock lock(z.airplay->mutex);
            z.airplay->members = std::move(members);
            z.airplay->states = std::move(states);
            z.airplay->refusal = std::move(refusal);
        }
        if (grouping) {
            syncZones();
            persist();
        }
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
            [this, zone = zoneId(Backend::Network, settings.network.udn)](const PlayerEvent& event) {
                followPlayer(zone, event);
            },
            options);
    }

    std::unique_ptr<audio::IOutput> hqplayerOutput(const Settings& settings) {
        if (settings.hqplayer.address.empty()) throw audio::Error(audio::ErrorCode::DeviceNotFound, "no HQPlayer chosen");
        return createHQPlayerOutput(
            settings.hqplayer.address, [this](const std::wstring& file) { return trackDetails(file); },
            [this, zone = zoneId(Backend::HQPlayer, settings.hqplayer.address)](const PlayerEvent& event) {
                followPlayer(zone, event);
            });
    }

    // Each zone's player's own volume is followed from when the zone is there, whether it plays or not.
    void followPlayerVolume() {
        for (auto& zone : zones_) {
            Zone& z = *zone;
            const bool own = z.settings.volume.mode == VolumeMode::Player;
            const std::string hqplayer = own && z.backend == Backend::HQPlayer ? z.output : "";
            if (hqplayer != z.volumeHQPlayer) {
                z.volumeHQPlayer = hqplayer;
                z.volumePlayer.reset();
                z.volumeUdn.clear();
                z.volume.reset();
                if (!hqplayer.empty()) z.volume = std::make_unique<HQPlayerVolume>(hqplayer, hqplayerVolumeOptions_);
            }
            if (!hqplayer.empty()) continue;
            const std::string udn = own && z.backend == Backend::Network ? z.output : "";
            if (udn == z.volumeUdn && state_.devices.revision == z.volumeDevices) continue;
            z.volumeUdn = udn;
            z.volumeDevices = state_.devices.revision;
            std::optional<upnp::PlayerDescription> player;
            if (!udn.empty()) {
                std::scoped_lock lock(finderMutex_);
                if (finder_)
                    for (auto& found : finder_->players())
                        if (found.description.udn == udn) player = std::move(found.description);
            }
            if (player == z.volumePlayer) continue;
            z.volume.reset();
            z.volumePlayer = std::move(player);
            if (!z.volumePlayer) continue;
            const upnp::PlayerDescription& described = *z.volumePlayer;
            if (described.service("RenderingControl")) z.volume = std::make_unique<PlayerVolume>(described);
        }
    }

    TrackDetails trackDetails(const std::wstring& file) {
        TrackDetails details;
        const std::string path = toUtf8(file);
        const auto now = snapshot();
        for (const auto& zone : now.zones) {
            const auto& entries = zone.queue.entries;
            const auto entry = std::ranges::find_if(entries, [&](const QueueEntry& e) { return e.source.pathUtf8() == path; });
            if (entry == entries.end()) continue;
            details.title = entry->title;
            details.artist = entry->artist;
            details.album = entry->album;
            break;
        }
        if (details.title.empty()) details.title = toUtf8(std::filesystem::path(file).stem().wstring());
        try {
            if (library_) {
                ArtworkRequest request;
                request.kind = ArtworkKind::Track;
                request.path = path;
                request.pixels = kPlayerCoverPixels;
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
    void followPlayer(const std::string& zone, const PlayerEvent& event) {
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
            const auto now = snapshot();
            const auto shown = std::ranges::find(now.zones, zone, &ZoneSnapshot::id);
            if (shown == now.zones.end()) return;
            const auto source = shown->playback.source;
            if (!source) return;
            command.kind = CommandKind::Seek;
            command.frame = event.positionMs * source->sampleRate / 1000;
            break;
        }
        default:
            return;
        }
        command.zone = zone;
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

    // What work that throws ends with, as a command's error would be.
    static std::optional<Failure> failureOf(const std::function<void()>& work) {
        try {
            work();
            return std::nullopt;
        } catch (const audio::Error& e) {
            return Failure{e.code(), e.what()};
        } catch (const std::exception& e) {
            return Failure{audio::ErrorCode::Other, e.what()};
        } catch (...) {
            return Failure{audio::ErrorCode::Other, "unknown Server command failure"};
        }
    }

    // Leaves the executing command's slow part to the deferred worker; `finish` then runs on this thread.
    void defer(std::function<void()> work, std::function<void()> finish = {}) {
        {
            std::lock_guard lock(mutex_);
            ++deferredOpen_;
        }
        {
            std::lock_guard lock(deferredMutex_);
            deferredQueue_.push_back({executing_, std::move(work), std::move(finish), std::nullopt});
        }
        deferredWake_.notify_all();
    }

    void deferredMain() {
        for (;;) {
            Deferred item;
            {
                std::unique_lock lock(deferredMutex_);
                while (!deferredStopping_ && deferredQueue_.empty()) deferredWake_.wait(lock);
                if (deferredQueue_.empty()) return;
                item = std::move(deferredQueue_.front());
                deferredQueue_.pop_front();
            }
            item.failure = failureOf(item.work);
            {
                std::lock_guard lock(mutex_);
                deferredDone_.push_back(std::move(item));
            }
            cv_.notify_all();
        }
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
                while (!closing_ && queue_.empty() && deferredDone_.empty())
                    if (cv_.wait_until(lock, due) == std::cv_status::timeout) break;
                if (!queue_.empty()) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                } else if (closing_)
                    break;
            }
            if (pending) {
                try {
                    for (auto& zone : zones_) {
                        updatePlaySession(*zone);
                        refreshLibraryAvailability(*zone);
                    }
                    followAirPlayGroup();
                    executing_ = pending->stamp.id;
                    execute(*pending);
                } catch (const audio::Error& e) {
                    state_.commandError = Failure{e.code(), e.what()};
                } catch (const std::exception& e) {
                    state_.commandError = Failure{audio::ErrorCode::Other, e.what()};
                } catch (...) {
                    state_.commandError = Failure{audio::ErrorCode::Other, "unknown Server command failure"};
                }
            }
            for (auto& zone : zones_) {
                try {
                    commitTransition(*zone);
                    refreshLibraryAvailability(*zone, true);
                    advanceAfterTrackEnd(*zone);
                    extendWithAutoplay(*zone);
                    ensureNextQueued(*zone);
                    recordPlaybackLength(*zone);
                    updatePlaySession(*zone);
                } catch (const std::exception& e) {
                    log::error("Server queue advance failed in zone %s: %s", zone->id.c_str(), e.what());
                }
            }
            try {
                followHolding();
                refreshDevicesAfterChange();
                followPlayerVolume();
                followAirPlayGroup();
                for (auto& zone : zones_)
                    if (zone->backend == Backend::AirPlay) resumeAsPairOnceFound(*zone);
            } catch (const std::exception& e) {
                log::error("Server queue advance failed: %s", e.what());
            }
            // One deferred command's end at a time, each in a snapshot of its own.
            std::optional<Deferred> done;
            {
                std::lock_guard lock(mutex_);
                if (!deferredDone_.empty()) {
                    done = std::move(deferredDone_.front());
                    deferredDone_.pop_front();
                }
            }
            if (done) {
                if (!done->failure && done->finish) done->failure = failureOf(done->finish);
                state_.finished = {done->command, std::move(done->failure)};
            }
            if (settingsDue_ && std::chrono::steady_clock::now() >= *settingsDue_) persist();
            for (auto& zone : zones_) saveQueueSession(*zone, false);
            keepAwakeWhilePlaying();
            publish(pending.has_value());
            if (pending || done) {
                {
                    std::lock_guard lock(mutex_);
                    if (pending) processed_ = pending->stamp.id;
                    if (done) --deferredOpen_;
                }
                idle_.notify_all();
            }
        }
        {
            std::lock_guard lock(deferredMutex_);
            deferredStopping_ = true;
        }
        deferredWake_.notify_all();
        deferredWorker_.join();
        deviceWatcher.reset();
        for (auto& zone : zones_) zone->volume.reset();
        {
            std::scoped_lock lock(finderMutex_);
            finder_.reset();
            airplayFinder_.reset();
            hqplayerFinder_.reset();
        }
        if (settingsDue_) persist();
        for (const auto& zone : zones_)
            if (zone->q.session) zone->q.session->persist(*library_);
        for (auto& zone : zones_) saveQueueSession(*zone, true);
        queueSaved_.set_value();
        backgroundStopped_.get_future().wait();
        {
            std::unique_lock zones(zonesMutex_);
            zones_.clear();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    struct LibraryEntry {
        std::wstring path;
        int64_t file = 0;
        bool detached = false;
        bool lengthRecorded = false;
        bool spelled = false;
    };
    // Gapless: the entry queued in the engine, and the last situation already tried so it is not retried every poll.
    struct QueueAttempt {
        uint64_t current = 0, upcoming = 0, revision = 0;
        size_t depth = 0;
        bool operator==(const QueueAttempt&) const = default;
    };
    struct QueueCheck {
        uint64_t revision = 0, current = 0;
        std::vector<uint64_t> chain;
        bool slotFree = false;
        bool operator==(const QueueCheck&) const = default;
    };
    // A zone's queue and what follows it, which goes with the queue to another output.
    struct ZoneQueue {
        QueuePolicy policy;
        std::vector<QueueEntry> entries;
        std::map<uint64_t, LibraryEntry> libraryEntries;
        uint64_t entryId = 0; // the current entry; 0 when none
        std::optional<TrackSource> track;
        std::optional<SavedTrack> restored; // the current entry restored (M5-R01) or left paused, not opened yet
        uint64_t autoplayTried = 0;         // the entry whose start autoplay already extended the queue for
        int failuresInARow = 0;             // tracks that failed one after another since one played or the user chose one
        std::optional<PlaySession> session;
        uint64_t sessionEntry = 0, sessionTransitions = 0, sessionTrackEnds = 0;
        std::wstring sessionFile;
        uint32_t sessionRate = 0;
        std::chrono::steady_clock::time_point sessionSeen = std::chrono::steady_clock::now(), sessionPersisted;
        bool sessionCountPersisted = false;
        int64_t libraryVersion = 0;
        int64_t libraryDeletions = -1; // entries whose files left the library leave the queue
        std::chrono::steady_clock::time_point availabilityChecked;
        uint64_t availabilityFiles = 0;
    };
    // An AirPlay zone's group as its output reads it, from any thread.
    struct AirPlayGroup {
        std::mutex mutex;
        _Guarded_by_(mutex) std::vector<AirPlayMember> members;
        _Guarded_by_(mutex) std::vector<ReceiverState> states;
        _Guarded_by_(mutex) std::string refusal; // why the group keeps this computer out; empty while it does not
        _Guarded_by_(mutex) std::vector<std::pair<std::string, audio::Error>> left;
        _Guarded_by_(mutex) std::vector<std::pair<std::string, airplay::ReceiverEvent>> volumes;
        // The receiver a request waits on for its user to allow CamelPlayer, since when.
        _Guarded_by_(mutex) std::optional<std::pair<std::string, std::chrono::steady_clock::time_point>> asking;
    };
    struct GroupBaseline {
        uint32_t master = 0;
        std::map<std::string, uint32_t> levels;
        std::chrono::steady_clock::time_point at;
    };
    // A zone's saved session (M5-R01): what was last written of it.
    struct SavedState {
        std::vector<uint64_t> entries, order, history;
        std::optional<uint64_t> current;
        bool shuffle = false;
        RepeatMode repeat = RepeatMode::Off;
        SavedTrack track;
        audio::PlayState playState = audio::PlayState::Stopped;
        std::chrono::steady_clock::time_point at;
    };
    // An enabled output, or the AirPlay group in use, playing with an Engine session, output and queue of its own
    // (docs/Plan.md §5.12).
    struct Zone {
        Backend backend = Backend::Wasapi;
        std::string output; // empty on WASAPI is "Windows default device"
        std::string id;     // zoneId(backend, output)
        Settings settings;  // the Server's with this output in use, as its engine has them
        std::unique_ptr<audio::PlaybackEngine> engine;
        ZoneQueue q;
        uint64_t seenTrackEnds = 0, seenTransitions = 0;
        std::deque<uint64_t> chain; // entries queued in the engine, in playback order
        QueueAttempt attempted;
        QueueCheck checked;
        bool queueChanged = false;            // queue state changed without a command or playback change
        uint64_t publishedEntry = UINT64_MAX; // the current entry `state.queue` was built for
        ZoneSnapshot state;                   // as last built for publishing
        std::filesystem::path queuedRead;     // the file last queued in the engine to follow; under readsMutex_
        bool sessionRestored = false;         // nothing is saved before its saved session is read back
        std::optional<std::chrono::steady_clock::time_point> holdingSince; // its device open, from when
        std::shared_ptr<AirPlayGroup> airplay = std::make_shared<AirPlayGroup>();
        std::optional<ReceiverLeft> receiverLeft;
        std::optional<std::chrono::steady_clock::time_point> pairedAt; // when its receiver went into a pair
        std::optional<GroupBaseline> groupBaseline;                    // the levels a moving group slider scales from
        std::unique_ptr<OwnVolume> volume;                             // its player's own, while followed
        std::string volumeHQPlayer;                                    // the address whose volume `volume` follows
        std::optional<upnp::PlayerDescription> volumePlayer;
        std::string volumeUdn;
        uint64_t volumeDevices = 0;
        SavedState saved;
        std::vector<size_t> savedRows; // rowHash() of each queue row as last written: rows before the first changed stay
        std::future<void> sessionWrite;
    };

    Settings settings_;
    std::filesystem::path settingsFile_;
    static constexpr auto kSettingsSettle = std::chrono::seconds(1);
    std::optional<std::chrono::steady_clock::time_point> settingsDue_; // when a change kept by persistSoon() is saved
    audio::OutputFactory factory_;
    mutable std::atomic<int> coversInFlight_{0}; // covers being read for the window; read by scan workers, so declared first
    std::atomic<int> pageLookups_{0};            // artist pages' own picture lookups going on
    std::unique_ptr<ArtworkCache> artwork_;
    std::unique_ptr<LibraryStore> library_;
    mutable std::mutex creditMutex_;
    _Guarded_by_(creditMutex_) mutable std::shared_ptr<const LibraryCreditNames> creditNames_;
    std::unique_ptr<LibraryTextIndex> libraryText_;
    std::unique_ptr<LibraryMonitor> libraryMonitor_;
    std::jthread coverWarmer_;   // uses artwork_, library_ and engine_, so it stops before they go
    std::jthread pictureFinder_; // uses library_ and the zones' reads, so it stops before they go
    mutable std::mutex loudnessMutex_;
    _Guarded_by_(loudnessMutex_) ReplayGainSettings loudnessSettings_;
    _Guarded_by_(loudnessMutex_) DecoderByFormat loudnessDecoders_;
    _Guarded_by_(loudnessMutex_) std::vector<int64_t> queuedFiles_;
    _Guarded_by_(loudnessMutex_) WaveformSettings waveformSettings_;
    _Guarded_by_(loudnessMutex_) std::vector<LoudnessAnalyzer::PlayingTrack> playingTracks_;
    std::unique_ptr<LoudnessAnalyzer> loudness_; // uses the four above, library_ and engine_
    std::optional<sqlite::Database> libraryReader_;
    mutable std::mutex readsMutex_; // each zone's queuedRead
    uint64_t marks_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_, idle_;
    _Guarded_by_(mutex_) std::deque<Pending> queue_;
    _Guarded_by_(mutex_) CommandStamp issued_;
    _Guarded_by_(mutex_) uint64_t processed_ = 0;
    _Guarded_by_(mutex_) bool closing_ = false;
    // A command's slow part, left beside the command thread so that no other command waits for it: pairing waits for a
    // receiver's user, and the library writer can be in a long transaction. Its `finish` then runs on the command
    // thread, and its outcome goes out as Snapshot::finished.
    struct Deferred {
        uint64_t command = 0;
        std::function<void()> work, finish;
        std::optional<Failure> failure;
    };
    _Guarded_by_(mutex_) std::deque<Deferred> deferredDone_;
    _Guarded_by_(mutex_) size_t deferredOpen_ = 0; // deferred and not yet finished, which waitIdle waits for
    std::mutex deferredMutex_;
    std::condition_variable deferredWake_;
    _Guarded_by_(deferredMutex_) std::deque<Deferred> deferredQueue_;
    _Guarded_by_(deferredMutex_) bool deferredStopping_ = false;
    std::thread deferredWorker_;
    uint64_t executing_ = 0; // the command being executed, on the command thread
    std::promise<void> queueSaved_, backgroundStopped_;
    Snapshot published_, state_;
    uint64_t lastEntryId_ = 0; // queue entry IDs are unique across zones
    uint64_t playSerial_ = 0, changeSerial_ = 0;
    _Guarded_by_(mutex_) std::optional<Caller> rebuiltBy_; // a library rebuild asked for since the last publish
    static constexpr size_t kAutoplayKept = 100;
    static constexpr int kAutoplayFailuresInARow = 3;
    std::vector<std::pair<uint64_t, uint64_t>> loudnessShared_; // each zone's queue revision and entry last shared
    // Changed on the command thread, which alone writes it; other threads read it shared.
    mutable std::shared_mutex zonesMutex_;
    std::vector<std::unique_ptr<Zone>> zones_; // in the output menu's order
    bool zonesChanged_ = false;                // zones added, removed or moved to another output since the last publish
    Zone* selected_ = nullptr;                 // the zone of the output in use at the last syncZones()
    bool sessionsRestored_ = false; // every zone's saved queue was read back at the start; a zone added later reads its own
    std::function<void(const Event&)> listener_;
    std::map<uint64_t, std::function<void(const Event&)>> observers_;
    uint64_t lastObserver_ = 0;
    std::unique_ptr<DiscordPresence> discord_;
    // Held shared by other threads while they use loudness_, libraryMonitor_ or discord_; shutdown takes them away.
    mutable std::shared_mutex partsMutex_;
    static constexpr std::chrono::seconds kDevicesSettle{1};
    static constexpr std::chrono::seconds kAskAgainAtPlay{3};
    static constexpr std::chrono::seconds kResumeAsPairWithin{60};
    std::atomic<int64_t> devicesChangedAt_{0}; // steady_clock ticks of the last device event; 0 when none is pending
    FinderOptions finderOptions_;
    std::function<LocalDevices()> localDevices_;
    std::mutex finderMutex_;
    _Guarded_by_(finderMutex_) std::unique_ptr<PlayerFinder> finder_; // from the first device scan
    hqplayer::FinderOptions hqplayerFinderOptions_;
    HQPlayerVolumeOptions hqplayerVolumeOptions_;
    _Guarded_by_(finderMutex_) std::unique_ptr<hqplayer::Finder> hqplayerFinder_; // from the first device scan
    AirPlayFinderOptions airplayFinderOptions_;
    std::map<std::string, std::string> airplaySupportLogged_; // by receiver: what the log last said of it
    static constexpr int kPairingTimeoutMs = 5000;
    static constexpr auto kChecksWait = std::chrono::seconds(25);
    std::pair<std::string, std::unique_ptr<airplay::RtspClient>> codeShown_; // the receiver showing a code, by ID
    static constexpr auto kAskingShownAfter = std::chrono::seconds(1);
    mutable std::mutex lyricsMutex_;
    _Guarded_by_(lyricsMutex_) mutable std::string lyricsPath_;
    _Guarded_by_(lyricsMutex_) mutable Lyrics lyrics_;
    static constexpr auto kGroupDragSettles = std::chrono::seconds(3);
    std::unique_ptr<AirPlayFinder> airplayFinder_; // from the first device scan
    std::shared_ptr<AirPlayPairings> airplayPairings_;
    bool keptAwake_ = false; // the worker thread keeps Windows from sleeping
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
void LocalServer::setHQPlayerFinderOptions(const hqplayer::FinderOptions& options) {
    impl_->setHQPlayerFinderOptions(options);
}
void LocalServer::setLocalDevices(std::function<LocalDevices()> list) {
    impl_->setLocalDevices(std::move(list));
}
BackgroundPace LocalServer::backgroundPace(const std::filesystem::path& storage) const {
    return impl_->backgroundPace(storage);
}
CommandStamp LocalServer::submit(Command command) {
    command.device = currentCaller().device;
    command.deviceName = currentCaller().name;
    return impl_->enqueue(std::move(command));
}
Snapshot LocalServer::snapshot() const {
    return impl_->snapshot();
}
uint64_t LocalServer::addObserver(std::function<void(const Event&)> observer) {
    return impl_->addObserver(std::move(observer));
}
void LocalServer::removeObserver(uint64_t id) {
    impl_->removeObserver(id);
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
std::optional<int64_t> LocalServer::albumPosition(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, bool favorites,
                                                  int64_t album, const CancellationToken& token) const {
    return camel::server::albumPosition(impl_->library(), sort, filter, favorites, album, token);
}
std::optional<int64_t> LocalServer::trackRowPosition(TrackSort sort, bool descending,
                                                     const std::optional<LibraryGroupFilter>& filter, int64_t file,
                                                     const CancellationToken& token) const {
    return camel::server::trackRowPosition(impl_->library(), sort, descending, filter, file, token);
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
    const auto page = impl_->pageLookup();
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
    const auto page = impl_->pageLookup();
    return camel::server::chooseArtistCandidate(impl_->library(), name, mbid, {}, token);
}

LibraryAlbumDetail LocalServer::albumDetail(int64_t album, const CancellationToken& token) const {
    return camel::server::albumDetail(impl_->library(), album, token);
}
std::vector<std::string> LocalServer::albumTrackPaths(const std::vector<int64_t>& albums, const CancellationToken& token) const {
    return camel::server::albumTrackPaths(impl_->library(), albums, token);
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
PlaylistChange LocalServer::saveQueueAsPlaylist(std::string name, const std::string& zone) {
    return impl_->saveQueueAsPlaylist(std::move(name), zone);
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
std::vector<ServerFolder> LocalServer::serverFolders(const std::string& pathUtf8) const {
    return foldersIn(pathUtf8);
}
std::string LocalServer::keepFile(const std::filesystem::path& file) {
    return toUtf8(file.wstring());
}
std::filesystem::path LocalServer::readableFile(const std::string& pathUtf8) const {
    return toWide(pathUtf8);
}
bool LocalServer::playerFirewallRule() const {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool found = firewallRuleExists();
    if (SUCCEEDED(com)) CoUninitialize();
    return found;
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
    // UI Automation runs only: AirPlay receivers are looked for on a loopback mDNS responder at this port alone.
    wchar_t port[16] = {};
    if (const DWORD length = GetEnvironmentVariableW(L"CAMELPLAYER_AIRPLAY_QUERY", port, DWORD(std::size(port)));
        length > 0 && length < std::size(port)) {
        AirPlayFinderOptions options;
        options.queryHost = "127.0.0.1";
        options.queryPort = uint16_t(std::wcstoul(port, nullptr, 10));
        options.groupPort = 0;
        options.searchInterval = std::chrono::seconds(2);
        server->setAirPlayFinderOptions(options);
    }
    return server;
}

} // namespace camel::server
