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
#include "Server/Library/ArtistPictures.h"
#include "Server/Library/LibraryBrowse.h"
#include "Server/Library/LibraryViews.h"
#include "Server/Library/PlayStatistics.h"
#include "Server/Library/Playlists.h"

namespace camel::server {

// Version 2 has zones (docs/Plan.md §5.12).
inline constexpr uint32_t kApiVersion = 2;
// The oldest version a Server still serves (docs/Plan.md §17.8): kept for a while after a new one, so updating either
// computer first keeps the link.
inline constexpr uint32_t kApiOldest = 2;

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
    bool convertWhenUnsupported = true;
    ResampleQuality resampleQuality = ResampleQuality::High;
    bool convertDsdWhenUnsupported = false;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    NetworkSettings network;
    AirPlaySettings airplay;
    HQPlayerSettings hqplayer;
    std::string language;
    CloseAction closeAction = CloseAction::Tray;
    Theme theme = Theme::System;
    bool trayNoticeShown = false;
    bool firstRunDone = false;
    bool autoplay = true;
    bool folderPlaysOn = false;
    bool checkUpdates = true;
    OnlineLookup artistPictures = OnlineLookup::Ask;
    bool folderTree = false;
    ScanSpeed scanSpeed = ScanSpeed::Low;
    WaveformSettings waveform;
    TrackColumns trackColumns;
    DiscordSettings discord;
    std::string skippedVersion;
    HotkeySettings hotkeys;
    EqSettings eq;
    CrossfeedSettings crossfeed;
    ConvolutionSettings convolution;
    audio::DspOrder dspOrder = audio::kDefaultDspOrder;
    VolumeSettings volume;
    ReplayGainSettings replayGain;
    CrossfadeSettings crossfade;
    std::vector<SavedOutput> outputs;
    std::vector<std::string> airplayGroup;
    std::string followedDefault;
    std::string lastFileUtf8;
    Role role = Role::Play;
    ControlledServer controlled;
    std::string followedZone;
    bool allowControl = false;
    std::string ownName;
    bool startWithWindowsAsked = false;
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
    AddLibraryRoot,    // path: a folder to index; overlapping and alias paths are normalized by the Server
    QueueAdd,          // sources: append entries without changing the current track
    QueuePlayNext,     // sources: insert entries after the current entry
    QueueRemove,       // entryIds: remove entries without stopping the current track
    QueueClear,        // keep the current queue entry, if any
    SetFavorite,       // tracks + favorite
    SetAlbumFavorite,  // albums + favorite: marks or clears every track of the albums
    SetArtistFavorite, // artists + favorite: artists and composers by name
    SetPlayerVolume,   // playerVolume: the network player's own level, 0-100; an AirPlay group's loudest receiver
    SetPlayerMute,     // playerMute: the network player's own mute; every receiver of an AirPlay group
    SetReceiverVolume, // receiver + playerVolume: one receiver of the AirPlay group in use
    ShowAirPlayCode,   // receiver: one that pairs with a code shows it
    PairAirPlay,       // receiver + code: pairs with the password or code the user typed
    DeleteOutput,      // zone: the output it names, enabled or not; its zone's queue goes and a zone playing stops
    PauseAll,          // every zone playing pauses
    Transfer,          // zone + target: the zone's queue, current entry and position go to the target zone
    GroupZones         // zone + zones: the zones that play with it from now on, of its kind; none ungroups it
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
    std::vector<LibraryGroupFilter> artists;
    std::optional<uint32_t> playerVolume;
    std::optional<bool> playerMute;
    std::string receiver; // an AirPlay receiver's device ID
    std::string code;     // a password or code an AirPlay receiver asks for
    // The zone a playback, queue or output command acts on, as ZoneSnapshot::id; empty for the selected zone. Other
    // commands name none.
    std::string zone;
    std::string target;             // Transfer: the zone that takes the queue
    std::vector<std::string> zones; // GroupZones: the zones that play with `zone`
    // Who sent it, which the Server takes from the call (Caller.h), never from the command: a paired device, or nobody
    // for its own window.
    std::string device, deviceName;
    bool reset = false; // ApplySettings: Settings › Reset playback and output, which the other devices are told of
    bool operator==(const Command&) const = default;
};

// Whether a command of `kind` acts on one zone.
bool actsOnZone(CommandKind kind);

enum class PlaybackState : uint8_t { Stopped, Playing, Paused };

// A line of a track's lyrics (Library/Lyrics.h).
struct LyricLine {
    std::optional<uint32_t> ms; // when it is sung; nothing in unsynced lyrics
    std::string text;
    bool operator==(const LyricLine&) const = default;
};

struct Lyrics {
    bool synced = false;
    std::vector<LyricLine> lines; // synced ones by time, the lines of one stamp in the order written; empty for none
    bool operator==(const Lyrics&) const = default;
};

// What a scan the user asked for did: started, or not, as one runs already or its library folders are offline.
enum class LibraryScanStart : uint8_t { Started, Running, Offline };

// A text tag of a file as its tag format holds it (Library/FileDetails.h).
struct FileTag {
    std::string key;   // as the format names it: TPE1, TXXX:MusicBrainz Album Id, ©ART, IART
    std::string field; // TagLib's property name, such as ARTIST; empty when it has none
    std::vector<std::string> values;
    std::optional<std::string> library; // what the library shows for the field, when that is not what this tag reads
    bool operator==(const FileTag&) const = default;
};

struct FileTagSection {
    std::string format; // ID3v2.4, ID3v1, APEv2, Vorbis comment, MP4, RIFF INFO, DIIN
    std::vector<FileTag> tags;
    bool operator==(const FileTagSection&) const = default;
};

struct FileDetails {
    enum class Problem : uint8_t { None, Missing, Unreadable };
    std::string path; // as the library spells it, else as asked for
    Problem problem = Problem::None;
    std::string reason;         // why the file could not be read
    bool folderPresent = false; // the file's folder can be opened
    bool inLibrary = false;
    std::vector<FileTagSection> sections;
    std::vector<FileTag> library; // the library's fields, when the file could not be read
    std::string format;           // formatName
    uint32_t sampleRate = 0, bitsPerSample = 0, channels = 0, bitrateKbps = 0;
    uint64_t frames = 0, lengthMs = 0;
    std::optional<uint64_t> size;
    std::optional<int64_t> modified;                                  // Unix seconds
    std::optional<double> trackGain, trackPeak, albumGain, albumPeak; // the ReplayGain tags as read
    std::optional<double> analysisTrackGain, analysisTrackPeak, analysisAlbumGain, analysisAlbumPeak; // the library's
};

// A receiver of the AirPlay group in use.
struct ReceiverState {
    std::string id, name;
    uint32_t level = 0; // 0-100
    bool muted = false;
    AirPlayChannels channels = AirPlayChannels::Stereo;
    bool operator==(const ReceiverState&) const = default;
};

// A member of a group of this computer's outputs (docs/Plan.md §5.12): its zone, whether the others keep to its clock,
// and why it stopped while they play on.
struct GroupMemberState {
    std::string id; // as ZoneSnapshot::id
    std::string name;
    bool clock = false;
    std::optional<audio::ErrorCode> lost;
    std::string detail;
    bool operator==(const GroupMemberState&) const = default;
};

// The last receiver to leave the group while the others played on.
struct ReceiverLeft {
    uint64_t serial = 0; // counts every departure, so the same receiver leaving again is told again
    std::string name;
    audio::ErrorCode code = audio::ErrorCode::DeviceLost;
    std::string detail;
    bool operator==(const ReceiverLeft&) const = default;
};

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
    std::optional<uint32_t> bufferingSeconds; // paused until a slow source is buffered: about this many seconds left
    bool waitingForPlayer = false;            // a network player asked to play that has not begun to
    std::optional<uint32_t> playerVolume;     // the network player's own level, 0-100; nothing without one to follow
    std::optional<double> playerVolumeDb;     // what `playerVolume` stands for on a player whose volume is in dB (HQPlayer)
    bool playerMuted = false;
    std::vector<ReceiverState> receivers;     // the AirPlay group in use, the one in use first; empty otherwise
    std::vector<GroupMemberState> members;    // a group of this computer's outputs, its first first; empty otherwise
    std::optional<ReceiverLeft> receiverLeft; // the last receiver that left the group
    std::string receiverAsking; // an AirPlay receiver's name while it has waited a second for its user to allow CamelPlayer
    std::optional<audio::ErrorCode> error;
    std::string errorDetail;
    uint64_t revision = 0;

    bool bitPerfect() const { return plan && plan->state == audio::PipelineState::BitPerfect; }
    bool operator==(const PlaybackSnapshot&) const = default;
};

struct DeviceInfo {
    std::string id, name;
    std::string unsupported;           // why an AirPlay receiver cannot be used, as airplay::supportId; empty when it can
    std::vector<std::string> speakers; // of an AirPlay stereo pair, as SavedOutput::speakers
    std::string model;                 // a network player's maker and model, as its description gives them
    std::string container;             // a local device's Windows container ID: the hardware it is part of
    std::string mac;                   // a network player's MAC address, from the ARP table; empty when unknown
    bool operator==(const DeviceInfo&) const = default;
};

struct DeviceCatalog {
    uint64_t revision = 0;
    std::vector<DeviceInfo> wasapi, asio, ks;
    std::vector<DeviceInfo> network;  // UPnP AV and OpenHome players, by UDN
    std::vector<DeviceInfo> airplay;  // AirPlay receivers, by device ID
    std::vector<DeviceInfo> hqplayer; // HQPlayers found or added that answer, by host:port
    std::string defaultWasapi;        // the Windows default render endpoint; empty when there is none
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

// The last command whose work went on beside the Server's command thread and has ended, and how: an AirPlay receiver
// showing its code or pairing, a change of the library. Such a command completes at once; this tells its outcome.
struct FinishedWork {
    uint64_t command = 0; // its CommandStamp::id; 0 before any
    std::optional<Failure> failure;
    bool operator==(const FinishedWork&) const = default;
};

// A zone (docs/Plan.md §5.12): an enabled output, or the AirPlay group in use, with a queue and playback of its own.
// What one device did to music others may be listening to (docs/Plan.md §17.8), shown on the devices it touches as
// "Paused by Study PC".
struct ZoneChange {
    uint64_t serial = 0; // the Server's count of changes; 0 when there is none
    // "paused", "stopped", "replaced", "transferred", "deleted", "statisticsReset", "libraryRebuilt", "playbackReset"
    std::string kind;
    std::string device, name; // a paired device; an empty device for the Server's own window
    std::string zone;         // the zone's name, where it is gone
    bool operator==(const ZoneChange&) const = default;
};

// The last Transfer to…: the devices that followed `from` follow `to` (docs/Plan.md §17.8).
struct ZoneTransfer {
    uint64_t serial = 0;
    std::string from, to;
    bool operator==(const ZoneTransfer&) const = default;
};

struct ZoneSnapshot {
    std::string id; // zoneId() of its output
    Backend backend = Backend::Wasapi;
    std::string output;        // the output's ID; empty on WASAPI is "Windows default device"
    std::string name;          // as the output menu shows it
    uint64_t queueEntryId = 0; // current queue entry; 0 when none
    std::optional<TrackSource> track;
    QueueSnapshot queue;
    PlaybackSnapshot playback;
    // The zone holding the device this zone plays to; Play here is refused with "in use by" that zone meanwhile.
    std::string inUseBy;
    uint64_t played = 0; // the Server's count of starts when it last started playing: the zone started last has the most
    ZoneChange change;   // the last pause, stop or new queue a device gave it
    bool operator==(const ZoneSnapshot&) const = default;
};

// "<backend ID>:<output ID>", the zone of that output.
std::string zoneId(Backend backend, const std::string& output);

struct Snapshot {
    uint32_t apiVersion = kApiVersion;
    uint64_t run = 0; // this start of the Server, which counts its revisions and commands from the start
    uint64_t revision = 0;
    CommandStamp command;
    // The selected zone's, as `zones` has them: the zone the window follows.
    uint64_t queueEntryId = 0; // current queue entry; 0 when none
    std::optional<TrackSource> track;
    QueueSnapshot queue;
    PlaybackSnapshot playback;
    std::vector<ZoneSnapshot> zones; // in the output menu's order
    std::string selectedZone;        // the zone of the output in use
    SettingsDto settings;
    DeviceCatalog devices;
    std::optional<Failure> commandError;
    FinishedWork finished;
    std::optional<bool> controlPanelOpened;
    ZoneTransfer transfer;
    ZoneChange notice; // the last change of no one zone: an output deleted, the library rebuilt or a reset
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

// A folder on the Server's computer, offered when a Control point adds a music folder (docs/Plan.md §17.8).
struct ServerFolder {
    std::string name;     // as listed: a drive with its label, a share or a folder's own name
    std::string pathUtf8; // what to open next or to add
    bool operator==(const ServerFolder&) const = default;
};

// What opening the data at startup had to repair, for the window to tell the user.
struct DataRecovery {
    SettingsRecovery settings = SettingsRecovery::None;
    enum class Library : uint8_t { None, RestoredBackup, Rebuilt } library = Library::None;
};

enum class DiscordConnection : uint8_t { Off, Searching, Connected };
enum class DiscordCover : uint8_t { Off, Downloading, Starting, Ready, Failed };
struct DiscordStatus {
    DiscordConnection connection = DiscordConnection::Off;
    DiscordCover cover = DiscordCover::Off;
    std::string coverError; // why the cloudflared download or start failed
    bool operator==(const DiscordStatus&) const = default;
};
struct DiscordTexts {
    std::string bitPerfect = "Bit-perfect", lossless = "Lossless conversion", processed = "Processed", lossy = "Lossy source";
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
    // Scan now (no folder) and Scan this folder (a folder of the Folders page): only what changed, at High.
    virtual LibraryScanStart scanLibraryNow(std::optional<int64_t> folder) = 0;
    virtual void clearArtworkCache() = 0;
    // Home (docs/Plan.md §8.2): albums by their latest play, newest first.
    virtual LibraryAlbumPage recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token = {}) const = 0;
    virtual LibraryAlbumPage browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                          int64_t limit, std::optional<uint64_t> revision,
                                          const CancellationToken& token = {}) const = 0;
    virtual LibraryGroupPage browseGroups(LibraryGroupField field, bool favoritesOnly, int64_t offset, int64_t limit,
                                          std::optional<uint64_t> revision, const CancellationToken& token = {}) const = 0;
    virtual std::vector<LibraryInitial> groupInitials(LibraryGroupField field, const CancellationToken& token = {}) const = 0;
    virtual std::vector<LibraryInitial> albumInitials(AlbumSort sort, const CancellationToken& token = {}) const = 0;
    // Each artist credit split into its names and the text between them; a part marked as a name opens that artist.
    virtual std::vector<std::vector<LibraryNamePart>> creditParts(const std::vector<std::string>& credits,
                                                                  const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackRowPage browseTrackRows(TrackSort sort, bool descending, const std::optional<LibraryGroupFilter>& filter,
                                                int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                                const CancellationToken& token = {}) const = 0;
    // Where an album stands in browseAlbums, or in favoriteAlbums with `favorites`; nothing when it is not listed.
    virtual std::optional<int64_t> albumPosition(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, bool favorites,
                                                 int64_t album, const CancellationToken& token = {}) const = 0;
    // Where the track of `file` stands in browseTrackRows; nothing when it is not listed.
    virtual std::optional<int64_t> trackRowPosition(TrackSort sort, bool descending,
                                                    const std::optional<LibraryGroupFilter>& filter, int64_t file,
                                                    const CancellationToken& token = {}) const = 0;
    // An artist's or composer's page: their ten most played tracks and the first albums of each listing.
    virtual LibraryArtistPage artistPage(const LibraryGroupFilter& artist, const CancellationToken& token = {}) const = 0;
    virtual LibraryTrackRowPage smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token = {}) const = 0;
    virtual LibraryAlbumPage favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token = {}) const = 0;
    virtual LibraryGroupPage favoriteArtists(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                             const CancellationToken& token = {}) const = 0;
    virtual std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token = {}) const = 0;
    // Artist pictures (docs/Plan.md §7.4), one per name for an artist and a composer of that name. With `lookUp` the
    // name is looked up first when due (see lookUpArtistPicture), online only while the settings allow it; `token`
    // stops that lookup.
    virtual ArtistPicture artistPicture(const std::string& name, bool lookUp, const CancellationToken& token = {}) = 0;
    // The user's own picture from an image file's bytes; throws std::invalid_argument when they are not a picture.
    virtual ArtistPicture setArtistPicture(const std::string& name, const std::vector<uint8_t>& image) = 0;
    virtual ArtistPicture removeArtistPicture(const std::string& name) = 0;
    // Which of the MusicBrainz artists sharing the name it is; looks that artist's picture up. Throws when the settings
    // do not allow online lookups or the lookup fails.
    virtual ArtistPicture chooseArtistCandidate(const std::string& name, const std::string& mbid,
                                                const CancellationToken& token = {}) = 0;
    virtual LibraryAlbumDetail albumDetail(int64_t album, const CancellationToken& token = {}) const = 0;
    // The paths of the albums' tracks, album after album, each in albumDetail's order.
    virtual std::vector<std::string> albumTrackPaths(const std::vector<int64_t>& albums,
                                                     const CancellationToken& token = {}) const = 0;
    virtual LibraryFolderListing browseFolder(std::optional<int64_t> folder, const CancellationToken& token = {}) const = 0;
    // Every track in a folder and below it, in the Folders page's order (see folderTracks in LibraryViews.h).
    virtual std::vector<LibraryTrackRow> folderTracks(int64_t folder, const CancellationToken& token = {}) const = 0;
    // A track and every track after it in its library folder, in the same order (see folderTracksFrom).
    virtual std::vector<LibraryTrackRow> folderTracksFrom(int64_t track, const CancellationToken& token = {}) const = 0;
    // The folders from a file's library folder down to its own (see fileFolders).
    virtual std::vector<int64_t> fileFolders(int64_t file) const = 0;
    virtual std::vector<LibraryRootInfo> libraryRoots() const = 0;
    virtual LibraryRootContents libraryRootContents(int64_t root) const = 0;
    virtual LibraryScanProgress libraryScanProgress() const = 0;
    // How far loudness analysis for ReplayGain has come (docs/Plan.md §5.9).
    virtual LoudnessProgress loudnessProgress() const = 0;
    virtual std::vector<PlaylistSummary> playlists() const = 0;
    virtual PlaylistPage playlistItems(int64_t playlist, int64_t offset, int64_t limit) const = 0;
    virtual SelectionPreview previewSelection(const std::vector<PlaylistSource>& sources,
                                              const CancellationToken& token = {}) const = 0;
    virtual PlaylistChange createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                          const CancellationToken& token = {}) = 0;
    virtual PlaylistChange addToPlaylist(int64_t playlist, std::optional<int64_t> revision,
                                         const std::vector<PlaylistSource>& sources, const CancellationToken& token = {}) = 0;
    // The zone's queue; an empty zone is the selected one.
    virtual PlaylistChange saveQueueAsPlaylist(std::string name, const std::string& zone = {}) = 0;
    virtual PlaylistChange editPlaylist(const PlaylistEdit& edit) = 0;
    virtual bool undoPlaylistChange(const PlaylistUndo& undo) = 0;
    virtual PlaylistChange importPlaylist(const std::filesystem::path& file) = 0;
    virtual void exportPlaylist(int64_t playlist, const std::filesystem::path& file) const = 0;
    // The diagnostic bundle (docs/Plan.md §17.5); probes the selected output device only while playback is stopped.
    virtual void writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths,
                                       const std::string& appVersion) const = 0;
    virtual DataRecovery dataRecovery() const = 0;
    // Discord Rich Presence (docs/Plan.md §8.4): how far the connection and the cover tunnel are.
    virtual DiscordStatus discordStatus() const = 0;
    // The lyrics of a track, read once per track from its tags or a .lrc beside it; the playing track's are kept.
    virtual Lyrics lyrics(const std::string& pathUtf8) const = 0;
    // Reads the file on the caller's thread, which may wait on a NAS; nothing is written to it.
    virtual FileDetails fileDetails(const std::string& pathUtf8) const = 0;
    // The track's waveform for the playback bar (Library/Waveform.h), once made; none while waveforms are off.
    virtual std::optional<std::vector<uint8_t>> waveform(const std::string& pathUtf8) const = 0;
    // The path states' names in the app's language, for the activity's small image; the Server has no strings of its own.
    virtual void setDiscordTexts(const DiscordTexts& texts) = 0;

    // Another computer's Server (docs/Plan.md §17.8): its files are not this computer's, so Explorer here cannot show
    // them, and a file chosen here is sent to it.
    virtual bool filesHere() const { return true; }
    // Whether it answers: another computer's Server may be lost for a while, or no longer know this device.
    // ServerTooOld and ThisTooOld: they share no API version, and the one named needs an update.
    enum class Link : uint8_t { Connected, Lost, Refused, ServerTooOld, ThisTooOld };
    virtual Link link() const { return Link::Connected; }
    // The Server's drives for an empty path, the shares of `\\computer`, else the folders in `pathUtf8`, by name; throws
    // std::runtime_error for one that cannot be listed. Its Windows user's rights decide what it reaches.
    virtual std::vector<ServerFolder> serverFolders(const std::string& pathUtf8) const = 0;
    // A file chosen on this computer as the Server reads it: the same path for its own window, a copy kept in its data
    // folder for a Control point.
    virtual std::string keepFile(const std::filesystem::path& file) = 0;
    // A file the Server keeps that this computer can read: the same path for its own window, a copy for a Control point.
    virtual std::filesystem::path readableFile(const std::string& pathUtf8) const = 0;
    // Whether Windows Firewall on the Server's computer has the rule network players need (docs/Plan.md §5.10).
    virtual bool playerFirewallRule() const = 0;
};

// Throws NewerSettingsError or NewerLibraryError for data a newer CamelPlayer wrote, and MigrationError or another
// exception when the data cannot be opened; nothing is changed then.
std::shared_ptr<IServer> createLocalServer();

} // namespace camel::server
