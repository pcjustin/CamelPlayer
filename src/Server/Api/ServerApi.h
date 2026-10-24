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
    SetShuffle, // shuffle
    SetRepeat,  // repeat
    RemoveLibraryRoot, // rootId
    ResetPlayStatistics, // tracks: delete their plays and zero their counters
    ResetAllPlayStatistics,
    AddLibraryRoot // path: a folder to index; overlapping and alias paths are normalized by the Server
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
    bool operator==(const QueueEntry&) const = default;
};

struct QueueSnapshot {
    uint64_t revision = 0; // QueuePolicy revision; prepared work for an older revision is stale
    std::vector<QueueEntry> entries;
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

class IServer {
public:
    virtual ~IServer() = default;
    virtual CommandStamp submit(Command command) = 0;
    virtual Snapshot snapshot() const = 0;
    // Invoked on the Server worker. Do not block or call waitIdle from a listener.
    virtual void setListener(std::function<void(const Event&)> listener) = 0;
    // Host/test barrier; UI code only submits commands and consumes snapshots/events.
    virtual void waitIdle() = 0;
    virtual LibraryTrackPage browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackPage searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibrarySearchResults searchLibrary(std::string query, int64_t offset, int64_t limit,
                                               std::optional<uint64_t> revision,
                                               const CancellationToken& token = {}) const = 0;
    virtual PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token = {}) const = 0;
    // Playlists (docs/Plan.md §7.9) run on the caller's thread and write through the library writer, so large
    // selections never hold up playback commands.
    // Browse views (docs/Plan.md §8.2); call them off the UI thread.
    virtual LibraryAlbumPage browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                          int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibraryGroupPage browseGroups(LibraryGroupField field, int64_t offset, int64_t limit,
                                          std::optional<uint64_t> revision, const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackRowPage browseTrackRows(TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                                std::optional<uint64_t> revision, const CancellationToken& token = {}) const = 0;
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
};

std::shared_ptr<IServer> createLocalServer();

} // namespace camel::server
