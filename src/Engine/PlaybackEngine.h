// Minimal playback engine of the MVP (docs/Plan.md §5.1, §16.2): decoder -> SPSC ring -> output.
// All commands run in order on one control thread; the UI only posts commands and reads snapshots.
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Common/AudioTypes.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/FormatRule.h"
#include "OutputBridge/DeviceWatcher.h"
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

enum class PlayState : uint8_t { Stopped, Playing, Paused };

struct EngineSnapshot {
    PlayState state = PlayState::Stopped;
    std::wstring file;
    uint64_t position = 0; // frames handed to the device
    uint64_t total = 0;
    std::optional<SourceInfo> source;
    std::optional<AudioFormat> format;  // probed from the current file
    std::optional<DeviceFormat> output; // set while a stream is open
    std::optional<OutputPlan> plan;     // how the source reaches `output`, and what that means for the indicator
    Backend backend = Backend::Wasapi;
    DecoderKind decoder = DecoderKind::Sndfile; // for the current file; FLAC's choice before a file is known
    OutputStatus outputStatus;
    uint64_t underruns = 0;
    std::optional<ErrorCode> error; // last problem, shown until the next successful start
    std::string errorDetail;
    uint64_t trackEnds = 0;    // increments when a playback attempt ends on its own: natural end or a stopping failure
    uint64_t transitions = 0;  // increments when a queued file becomes the current one without reopening the device
    size_t queuedFiles = 0;    // queued files not yet audible (at most two)
    bool nextSlotFree = false; // another file may be queued now
    uint64_t revision = 0;     // increments on every change

    bool bitPerfect() const { return plan && plan->state == PipelineState::BitPerfect; }
};

using OutputFactory = std::function<std::unique_ptr<IOutput>(const Settings&)>;

class PlaybackEngine {
public:
    // `watcher` reports Windows default-device changes while WASAPI follows the default.
    explicit PlaybackEngine(const Settings& settings, OutputFactory factory = createOutput,
                            DeviceWatcherFactory watcher = watchDefaultRenderDevice);
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    // `decoder` overrides the per-format setting for this file; it must support the file's format.
    void openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt);
    // Selects a file without playing it (startup restore, docs/Plan.md §2); play() starts it.
    void load(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt);
    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(uint64_t frame);
    // Applies immediately: an open stream restarts from the current position (docs/Plan.md §2).
    void applySettings(const Settings& settings);
    // ASIO only: opens the driver's control panel. Blocks until the driver returns.
    bool showControlPanel();
    // Gapless: queues the file that follows the current one. True when it will play without reopening the device,
    // which needs the Rust Engine and the same negotiated output format.
    bool queueNext(const std::filesystem::path& file, std::optional<DecoderKind> decoder = std::nullopt);
    // Withdraws the queued file; false when it is already committed and becomes current at its boundary.
    bool clearNext();

    EngineSnapshot snapshot() const;
    // Called on the engine thread after every change; keep it short (e.g. post to the UI thread).
    void setListener(std::function<void()> listener);

    // Blocks until every command posted so far has run.
    void waitIdle();
    // Blocks until the state equals `s` or the timeout passes.
    bool waitForState(PlayState s, std::chrono::milliseconds timeout);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::audio
