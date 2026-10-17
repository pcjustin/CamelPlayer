// One playback session on the Rust Engine through its versioned C ABI (docs/Engine-Bridge-ABI.md).
// The engine is created for one negotiated output format; seek or a format change opens a new session.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "audio/AudioTypes.h"
#include "audio/DecoderKind.h"
#include "audio/engine/EngineAbi.h"

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

private:
    CamelCommandStampV1 nextCommand();

    CamelEngine* engine_ = nullptr;
    CamelDecoderHostV1 host_{};
    CamelCommandStampV1 command_{};
};

} // namespace camel::audio
