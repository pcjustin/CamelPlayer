// Minimal playback engine of the MVP (docs/Plan.md §5.1, §16.2): decoder -> SPSC ring -> output.
// All commands run in order on one control thread; the UI only posts commands and reads snapshots.
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "audio/AudioTypes.h"
#include "audio/decode/IDecoder.h"
#include "audio/output/IOutput.h"
#include "settings/Settings.h"

namespace camel::audio {

enum class PlayState : uint8_t { Stopped, Playing, Paused };

struct EngineSnapshot {
    PlayState state = PlayState::Stopped;
    std::wstring file;
    uint64_t position = 0; // frames handed to the device
    uint64_t total = 0;
    std::optional<SourceInfo> source;
    std::optional<DeviceFormat> output; // set while a stream is open; always bit-perfect under the MVP rule
    Backend backend = Backend::Wasapi;
    DecoderKind decoder = DecoderKind::Sndfile;
    OutputStatus outputStatus;
    uint64_t underruns = 0;
    std::optional<ErrorCode> error; // last problem, shown until the next successful start
    std::string errorDetail;
    uint64_t trackEnds = 0; // increments when a playback attempt ends on its own: natural end or a stopping failure
    uint64_t revision = 0;  // increments on every change

    bool bitPerfect() const { return output.has_value(); }
};

using OutputFactory = std::function<std::unique_ptr<IOutput>(const Settings&)>;

class PlaybackEngine {
public:
    explicit PlaybackEngine(const Settings& settings, OutputFactory factory = createOutput);
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    void openAndPlay(const std::filesystem::path& file);
    // Selects a file without playing it (startup restore, docs/Plan.md §2); play() starts it.
    void load(const std::filesystem::path& file);
    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(uint64_t frame);
    // Applies immediately: an open stream restarts from the current position (docs/Plan.md §2).
    void applySettings(const Settings& settings);
    // ASIO only: opens the driver's control panel. Blocks until the driver returns.
    bool showControlPanel();

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
