#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/FormatRule.h"
#include "Server/Playback/QueuePolicy.h"
#include "Common/settings/Settings.h"

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
    SetRepeat   // repeat
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
    std::optional<bool> shuffle;
    std::optional<RepeatMode> repeat;
};

enum class PlaybackState : uint8_t { Stopped, Playing, Paused };

struct Failure {
    audio::ErrorCode code = audio::ErrorCode::Other;
    std::string detail;
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
    uint32_t bufferFrames = 0;
    uint64_t underruns = 0;
    std::optional<audio::ErrorCode> error;
    std::string errorDetail;
    uint64_t revision = 0;

    bool bitPerfect() const { return plan && plan->state == audio::PipelineState::BitPerfect; }
};

struct DeviceInfo {
    std::string id, name;
};

struct DeviceCatalog {
    uint64_t revision = 0;
    std::vector<DeviceInfo> wasapi, asio, ks;
};

struct QueueEntry {
    uint64_t id = 0;
    TrackSource source;
};

struct QueueSnapshot {
    uint64_t revision = 0; // QueuePolicy revision; prepared work for an older revision is stale
    std::vector<QueueEntry> entries;
    std::optional<uint64_t> upcomingEntryId; // what a natural completion of the current entry plays next
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
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
};

enum class EventKind : uint8_t { StateChanged, CommandCompleted };
struct Event {
    EventKind kind = EventKind::StateChanged;
    Snapshot snapshot;
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
};

std::shared_ptr<IServer> createLocalServer();

} // namespace camel::server
