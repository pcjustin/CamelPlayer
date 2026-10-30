// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "Server/Artwork/ArtworkCache.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/FormatRule.h"
#include "Server/Playback/QueuePolicy.h"
#include "Common/settings/Settings.h"
#include "Server/Library/LibraryBrowse.h"
#include "Server/Library/LibraryViews.h"
#include "Server/Library/PlayStatistics.h"
#include "Server/Library/Playlists.h"

namespace camel::server {

inline constexpr uint32_t kApiVersion = 1;

struct FrameRange {
    uint64_t first = 0;
    std::optional<uint64_t> endExclusive;
    bool operator==(const FrameRange&) const = default;
};

class TrackSource {
public:
    explicit TrackSource(std::string pathUtf8, FrameRange range = {}, std::optional<audio::DecoderKind> decoder = std::nullopt);
    const std::string& pathUtf8() const { return pathUtf8_; }
    const FrameRange& range() const { return range_; }
    std::optional<audio::DecoderKind> decoder() const { return decoder_; }
    bool operator==(const TrackSource&) const = default;

private:
    std::string pathUtf8_;
    FrameRange range_;
    std::optional<audio::DecoderKind> decoder_;
};

struct SettingsDto {
    Backend backend = Backend::Wasapi;
    DecoderByFormat decoderByFormat;
    PcmBitsByFormat pcmBitsByFormat;
    bool convertWhenUnsupported = false;
    ResampleQuality resampleQuality = ResampleQuality::High;
    bool convertDsdWhenUnsupported = false;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    std::string language;
    CloseAction closeAction = CloseAction::Tray;
    Theme theme = Theme::System;
    bool trayNoticeShown = false;
    bool firstRunDone = false;
    bool autoplay = true;
    bool checkUpdates = true;
    std::string skippedVersion;
    HotkeySettings hotkeys;
    std::string lastFileUtf8;
    bool operator==(const SettingsDto&) const = default;
};

SettingsDto settingsDto(const Settings& s);
Settings nativeSettings(const SettingsDto& s);

struct CommandStamp {
    uint64_t id = 0;
    uint64_t generation = 0;
    uint64_t queueRevision = 0;
    bool operator==(const CommandStamp&) const = default;
};

enum class CommandKind : uint8_t {
    OpenAndPlay,
    Load,
    Play,
    Pause,
    TogglePause,
    Stop,
    Seek,
    ApplySettings,
    RefreshDevices,
    ShowControlPanel,
    QueueReplace, // sources + index: replace the queue and play sources[index]
    QueueReorder, // entryIds: a permutation of the current queue entries
    QueueSelect,  // entryId: play that queue entry
    Next,
    Previous,
    SetShuffle,          // shuffle
    SetRepeat,           // repeat
    RemoveLibraryRoot,   // rootId
    ResetPlayStatistics, // tracks: delete their plays and zero their counters
    ResetAllPlayStatistics,
    AddLibraryRoot,  // path: a folder to index; overlapping and alias paths are normalized by the Server
    QueueAdd,        // sources: append entries without changing the current track
    QueuePlayNext,   // sources: insert entries after the current entry
    QueueRemove,     // entryIds: remove entries without stopping the current track
    QueueClear,      // keep the current queue entry, if any
    SetFavorite,     // tracks + favorite
    SetAlbumFavorite // albums + favorite: marks or clears every track of the albums
};

struct Command {
    uint32_t apiVersion = kApiVersion;
    CommandKind kind = CommandKind::Stop;
    std::optional<TrackSource> source;
    std::optional<SettingsDto> settings;
    uint64_t frame = 0;
    std::vector<TrackSource> sources;
    uint64_t index = 0;
    std::vector<uint64_t> entryIds;
    uint64_t entryId = 0;
    int64_t rootId = 0;
    std::optional<bool> shuffle;
    std::optional<RepeatMode> repeat;
    std::vector<LibraryTrackKey> tracks;
    std::string path;
    bool favorite = false;
    std::vector<int64_t> albums;
    bool operator==(const Command&) const = default;
};

enum class PlaybackState : uint8_t { Stopped, Playing, Paused };

struct Failure {
    audio::ErrorCode code = audio::ErrorCode::Other;
    std::string detail;
    bool operator==(const Failure&) const = default;
};

struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Stopped;
    std::string pathUtf8;
    uint64_t position = 0;
    uint64_t total = 0;
    std::optional<audio::SourceInfo> source;
    std::optional<audio::AudioFormat> format;
    std::optional<audio::DeviceFormat> output;
    std::optional<audio::OutputPlan> plan;
    std::optional<audio::Md5State> md5; // FLAC only
    Backend backend = Backend::Wasapi;
    audio::DecoderKind decoder = audio::DecoderKind::Sndfile;
    std::optional<Backend> activeBackend;
    std::optional<audio::DecoderKind> activeDecoder;
    std::string outputMode, outputDetail;
    std::vector<std::string> outputAdjustments; // saved output parameters replaced by device defaults (requested ≠ active)
    uint32_t bufferFrames = 0;
    uint64_t underruns = 0;
    std::optional<audio::ErrorCode> error;
    std::string errorDetail;
    uint64_t revision = 0;

    bool bitPerfect() const { return plan && plan->state == audio::PipelineState::BitPerfect; }
    bool operator==(const PlaybackSnapshot&) const = default;
};

struct DeviceInfo {
    std::string id, name;
    bool operator==(const DeviceInfo&) const = default;
};

struct DeviceCatalog {
    uint64_t revision = 0;
    std::vector<DeviceInfo> wasapi, asio, ks;
    bool operator==(const DeviceCatalog&) const = default;
};

struct QueueEntry {
    uint64_t id = 0;
    TrackSource source;
    std::string title, artist, album;
    int64_t albumId = 0;
    bool automatic = false; // added by autoplay when the queue ended (docs/Plan.md §8.1)
    int64_t file = 0;       // the library file, 0 outside the library
    bool favorite = false;
    bool operator==(const QueueEntry&) const = default;
};

// The queue's entries, shared by the snapshots that show them: snapshots are copied several times a second and a queue
// can hold a whole 100k-track library.
class QueueEntries {
public:
    QueueEntries() = default;
    QueueEntries(std::vector<QueueEntry> entries)
        : entries_(std::make_shared<const std::vector<QueueEntry>>(std::move(entries))) {}
    QueueEntries(std::initializer_list<QueueEntry> entries) : QueueEntries(std::vector<QueueEntry>(entries)) {}
    const std::vector<QueueEntry>& get() const {
        static const std::vector<QueueEntry> none;
        return entries_ ? *entries_ : none;
    }
    auto begin() const { return get().begin(); }
    auto end() const { return get().end(); }
    size_t size() const { return get().size(); }
    bool empty() const { return get().empty(); }
    const QueueEntry& operator[](size_t index) const { return get()[index]; }
    const QueueEntry& front() const { return get().front(); }
    const QueueEntry& back() const { return get().back(); }
    bool operator==(const QueueEntries& other) const { return entries_ == other.entries_ || get() == other.get(); }

private:
    std::shared_ptr<const std::vector<QueueEntry>> entries_;
};

struct QueueSnapshot {
    uint64_t revision = 0; // QueuePolicy revision; prepared work for an older revision is stale
    uint64_t marks = 0;    // changes when the entries' favorite marks change
    QueueEntries entries;
    std::optional<uint64_t> upcomingEntryId; // what a natural completion of the current entry plays next
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    bool operator==(const QueueSnapshot&) const = default;
};

struct Snapshot {
    uint32_t apiVersion = kApiVersion;
    uint64_t revision = 0;
    CommandStamp command;
    uint64_t queueEntryId = 0; // current queue entry; 0 when none
    std::optional<TrackSource> track;
    QueueSnapshot queue;
    PlaybackSnapshot playback;
    SettingsDto settings;
    DeviceCatalog devices;
    std::optional<Failure> commandError;
    std::optional<bool> controlPanelOpened;
    bool operator==(const Snapshot&) const = default;
};

enum class EventKind : uint8_t { StateChanged, CommandCompleted };
struct Event {
    EventKind kind = EventKind::StateChanged;
    Snapshot snapshot;
    bool operator==(const Event&) const = default;
};

class SnapshotCursor {
public:
    void expect(CommandStamp command);
    bool accept(const Snapshot& snapshot);
    const std::optional<Snapshot>& current() const { return current_; }

private:
    CommandStamp expected_;
    std::optional<Snapshot> current_;
};

// What opening the data at startup had to repair, for the window to tell the user.
struct DataRecovery {
    SettingsRecovery settings = SettingsRecovery::None;
    enum class Library : uint8_t { None, RestoredBackup, Rebuilt } library = Library::None;
};

class IServer {
public:
    virtual ~IServer() = default;
    virtual CommandStamp submit(Command command) = 0;
    virtual Snapshot snapshot() const = 0;
    // Invoked on the Server worker. Do not block or call waitIdle from a listener.
    virtual void setListener(std::function<void(const Event&)> listener) = 0;
    // Host/test barrier; UI code only submits commands and consumes snapshots/events.
    virtual void waitIdle() = 0;
    // Called by the host once its UI is gone: ends playback and background work, saves the queue and closes the
    // library. The object stays usable for reads, since parts of the UI may still hold it.
    virtual void shutdown() = 0;
    virtual LibraryTrackPage browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackPage searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    // totals=false leaves the totals 0 (see LibraryBrowse.h).
    virtual LibrarySearchResults searchLibrary(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                               const CancellationToken& token = {}, bool totals = true) const = 0;
    virtual PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token = {}) const = 0;
    // Playlists (docs/Plan.md §7.9) run on the caller's thread and write through the library writer, so large
    // selections never hold up playback commands.
    // Browse views (docs/Plan.md §8.2); call them off the UI thread.
    // Settings › Reset (docs/Plan.md §8.2): reread every tag of every root, keeping play statistics and playlists;
    // delete the generated cover thumbnails.
    virtual void rereadLibrary() = 0;
    virtual void clearArtworkCache() = 0;
    // Home (docs/Plan.md §8.2): albums by their latest play, newest first.
    virtual LibraryAlbumPage recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token = {}) const = 0;
    virtual LibraryAlbumPage browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                          int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibraryGroupPage browseGroups(LibraryGroupField field, int64_t offset, int64_t limit,
                                          std::optional<uint64_t> revision, const CancellationToken& token = {}) const = 0;
    virtual std::vector<LibraryInitial> groupInitials(LibraryGroupField field, const CancellationToken& token = {}) const = 0;
    virtual std::vector<LibraryInitial> albumInitials(AlbumSort sort, const CancellationToken& token = {}) const = 0;
    // Each artist credit split into its names and the text between them; a part marked as a name opens that artist.
    virtual std::vector<std::vector<LibraryNamePart>> creditParts(const std::vector<std::string>& credits,
                                                                  const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackRowPage browseTrackRows(TrackSort sort, bool descending, const std::optional<LibraryGroupFilter>& filter,
                                                int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                                const CancellationToken& token = {}) const = 0;
    // An artist's or composer's page: their ten most played tracks and the first albums of each listing.
    virtual LibraryArtistPage artistPage(const LibraryGroupFilter& artist, const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackRowPage smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token = {}) const = 0;
    virtual LibraryAlbumPage favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token = {}) const = 0;
    virtual std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token = {}) const = 0;
    virtual LibraryAlbumDetail albumDetail(int64_t album, const CancellationToken& token = {}) const = 0;
    virtual LibraryFolderListing browseFolder(std::optional<int64_t> folder, const CancellationToken& token = {}) const = 0;
    virtual std::vector<LibraryRootInfo> libraryRoots() const = 0;
    virtual LibraryScanProgress libraryScanProgress() const = 0;
    virtual std::vector<PlaylistSummary> playlists() const = 0;
    virtual PlaylistPage playlistItems(int64_t playlist, int64_t offset, int64_t limit) const = 0;
    virtual SelectionPreview previewSelection(const std::vector<PlaylistSource>& sources,
                                              const CancellationToken& token = {}) const = 0;
    virtual PlaylistChange createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                          const CancellationToken& token = {}) = 0;
    virtual PlaylistChange addToPlaylist(int64_t playlist, std::optional<int64_t> revision,
                                         const std::vector<PlaylistSource>& sources, const CancellationToken& token = {}) = 0;
    virtual PlaylistChange saveQueueAsPlaylist(std::string name) = 0;
    virtual PlaylistChange editPlaylist(const PlaylistEdit& edit) = 0;
    virtual bool undoPlaylistChange(const PlaylistUndo& undo) = 0;
    virtual PlaylistChange importPlaylist(const std::filesystem::path& file) = 0;
    virtual void exportPlaylist(int64_t playlist, const std::filesystem::path& file) const = 0;
    // The diagnostic bundle (docs/Plan.md §17.5); probes the selected output device only while playback is stopped.
    virtual void writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths,
                                       const std::string& appVersion) const = 0;
    virtual DataRecovery dataRecovery() const = 0;
};

// Throws NewerSettingsError or NewerLibraryError for data a newer CamelPlayer wrote, and MigrationError or another
// exception when the data cannot be opened; nothing is changed then.
std::shared_ptr<IServer> createLocalServer();

} // namespace camel::server
