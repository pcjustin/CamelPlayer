// One playback session on the Rust Engine through its versioned C ABI (docs/Engine-Bridge-ABI.md).
// The engine is created for one negotiated output format; seek or a format change opens a new session.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

class RustSession {
public:
    // Loads `path` with `decoder` for output `format`, positioned at `startFrame`. Throws Error on failure.
    RustSession(const std::filesystem::path& path, DecoderKind decoder, const DeviceFormat& format, uint64_t startFrame);
    ~RustSession();
    RustSession(const RustSession&) = delete;
    RustSession& operator=(const RustSession&) = delete;

    void play();
    void pause();

    enum class Prefill : uint8_t { Decoded, Full, End };
    // Decode worker only. Throws Error when decoding fails.
    Prefill prefill(uint64_t& frames);

    struct Rendered {
        size_t frames = 0;
        bool ended = false; // end of stream reached and fully drained
        bool underrun = false;
        bool fault = false; // the engine stopped after a decode failure
    };
    // Output callback only; never blocks or allocates.
    Rendered render(uint8_t* destination, size_t frames, size_t bytesPerFrame) noexcept;

    // The output bridge's estimate of what the listener hears now.
    void reportAudible(uint64_t frame);

    // Gapless: the next track plays without reopening the device. False when it does not decode to this session's
    // output format or cannot be opened; playback then ends normally after the current track.
    bool queueNext(const std::filesystem::path& path, DecoderKind decoder);
    // False once decoding has moved on to the queued track; it then becomes current at its boundary.
    bool clearNext();

    struct Transitions {
        uint64_t count = 0;         // queued tracks that have become audible
        uint64_t trackPosition = 0; // frames of the current track handed to the device
        bool nextQueued = false;    // a queued track still waits in the slot; another may be queued only when false
    };
    // Safe on the output callback thread.
    Transitions transitions() const noexcept;

private:
    CamelCommandStampV1 nextCommand();

    CamelEngine* engine_ = nullptr;
    CamelDecoderHostV1 host_{};
    CamelCommandStampV1 command_{};
};

} // namespace camel::audio
