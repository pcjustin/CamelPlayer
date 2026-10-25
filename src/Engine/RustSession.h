// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// One playback session on the Rust Engine through its versioned C ABI (docs/Engine-Bridge-ABI.md).
// The engine is created for one negotiated output format; seek or a format change opens a new session.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"
#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"
#include "Engine/FormatRule.h"

namespace camel::audio {

class RustSession {
public:
    // Whether the Engine may resample, downmix or reduce depth, and with which soxr quality.
    struct Conversion {
        bool allow = false;
        ResampleQuality quality = ResampleQuality::High;
        bool allowDsd = false; // DSD to PCM
    };

    // Loads `path` with `decoder` for output `format`, positioned at `startFrame`; lossy files, including queued ones,
    // decode to the depth `pcmBits` sets. `dsd` makes the stream native DSD or DoP. The Engine predecodes up to
    // `predecodeSeconds` (within its memory limit). Throws Error on failure.
    RustSession(const std::filesystem::path& path, DecoderKind decoder, const DeviceFormat& format, uint64_t startFrame,
                const PcmBitsByFormat& pcmBits, Conversion conversion, DsdPath dsd = DsdPath::None,
                uint32_t predecodeSeconds = 10);
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
    // Output callback only; never blocks or allocates. A raw DSD stream fills all `frames` (silence after the audio).
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

    // For the audible track, even while the decoder already reads the queued one.
    Md5State md5State() const noexcept;

private:
    CamelCommandStampV1 nextCommand();

    CamelEngine* engine_ = nullptr;
    PcmBitsByFormat pcmBits_; // read by the decoder host; declared before host_, which points to it
    CamelDecoderHostV1 host_{};
    CamelCommandStampV1 command_{};
    bool raw_ = false;           // DoP or native DSD
    uint64_t unitsPerFrame_ = 1; // render units per device frame: 8 DSD samples per native byte frame
};

} // namespace camel::audio
