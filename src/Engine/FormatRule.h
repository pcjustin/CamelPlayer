// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Output format negotiation (docs/Plan.md §5.3) and what the chosen path means for the indicator (§5.2).
#pragma once
#include <functional>
#include <optional>
#include <vector>

#include "Common/AudioTypes.h"

namespace camel::audio {

// Bit-perfect: every source bit reaches the device (zero padding, mono duplication). Lossless conversion: integers
// widened to float32 for a float-only device. Processed: resampled, downmixed or reduced in depth. Lossy source: an
// AAC/MP3 decode passed on unchanged otherwise.
enum class PipelineState : uint8_t { BitPerfect, LosslessConversion, Processed, LossySource };

// How a DSD source reaches the device: native DSD bytes, DoP in a 24-bit PCM carrier, or converted to PCM
// (docs/Plan.md §5.8).
enum class DsdPath : uint8_t { None, Native, Dop, Pcm };

// Why the change into a track did not crossfade while crossfade is on (docs/Plan.md §5.4): raw DSD on either side, a
// format the open stream could not take, or no next track ready in time.
enum class FadeSkip : uint8_t { None, RawDsd, Format, NoPreload };

struct OutputPlan {
    DeviceFormat format;
    bool duplicate = false; // mono to both channels
    bool downmix = false;   // ITU-R BS.775 to stereo
    bool resample = false;
    bool widen = false;  // integers to float32
    bool reduce = false; // fewer bits than the source, with dither
    DsdPath dsd = DsdPath::None;
    bool remodulate = false;  // raw DSD at a lower rate than the source's, modulated again
    bool shared = false;      // through the WASAPI shared-mode mixer
    std::vector<DspNode> dsp; // the DSP nodes that change the signal, in order (docs/Plan.md §5.9)
    double gainDb = 0.0;      // the track's ReplayGain
    double volumeDb = 0.0;    // the software volume
    bool muted = false;
    double crossfadeSeconds = 0.0; // the crossfade at track changes; 0 while off or for raw DSD
    FadeSkip fadeSkip = FadeSkip::None;
    PipelineState state = PipelineState::BitPerfect;
    bool operator==(const OutputPlan&) const = default;
};

// Containers tried for a source of `bits` bits, smallest first: 16 -> 24 -> 24-in-32 -> 32.
std::vector<SampleType> bitPerfectCandidates(uint32_t bits);

// Rates are tried in order: the source rate, then with `convert` the lowest same-family rate at or above it, the
// lowest other-family rate at or above it, and the highest rate. Each rate tries containers that carry the source
// depth, then float32, then with `convert` a smaller depth. Output is always stereo: mono is duplicated, more channels
// need `convert`. `fixedType` (from settings) restricts the container. `lossy` marks a float decode (AAC/MP3). A
// nonzero `fixedRate` is the only rate tried, resampled without `convert`. A source above a nonzero `maxRate` (the
// output's highest PCM rate) is first tried at limitedRate, resampled without `convert`. Returns nullopt when nothing
// fits; the caller then skips the track and tells the user.
std::optional<OutputPlan> negotiateOutput(const SourceInfo& src, bool lossy, std::optional<SampleType> fixedType, bool convert,
                                          const std::function<bool(const DeviceFormat&)>& deviceSupports, uint32_t fixedRate = 0,
                                          uint32_t maxRate = 0);

// The highest rate of `rate`'s family (44.1 or 48 kHz) at or below `maxRate`, else the highest of either: with
// 192 kHz, 352.8 and 705.6 kHz become 176.4 kHz and 384 and 768 kHz become 192 kHz. `rate` itself when not above.
uint32_t limitedRate(uint32_t rate, uint32_t maxRate);

// Raw DSD (`src` from the DSD Nexus decoder: byte frames, 1 bit per sample), docs/Plan.md §5.3 item 4: native DSD
// when `native` is requested and the device takes it, else DoP at DSD rate / 16 in a 24-bit, 24-in-32 or 32-bit
// carrier (`fixedType` restricts it). Never PCM; mono is duplicated, more channels are refused. A carrier above
// 768 kHz (DSD512 DoP needs 1411.2 kHz) is used only if the device reports it. A source above a nonzero
// `maxByteRate` (the output's highest DSD rate, in byte frames per second) is sent at that rate, and while the DoP
// carrier of the rate tried is above a nonzero `maxPcmRate` the rate steps down to DSD64; a rate below the source's
// is modulated again (`remodulate`) and Processed.
std::optional<OutputPlan> negotiateDsd(const SourceInfo& src, bool native, std::optional<SampleType> fixedType,
                                       const std::function<bool(const DeviceFormat&)>& deviceSupports, uint32_t maxByteRate = 0,
                                       uint32_t maxPcmRate = 0);

// DSD converted to PCM: decimated to 352.8 kHz, then delivered like a 24-bit source at `rate` (the setting), with
// `convert` governing other rates, downmix and depth exactly as for PCM, and `maxRate` as for negotiateOutput. Always
// Processed; nullopt when nothing fits.
std::optional<OutputPlan> negotiateDsdToPcm(const SourceInfo& src, uint32_t rate, std::optional<SampleType> fixedType,
                                            bool convert, const std::function<bool(const DeviceFormat&)>& deviceSupports,
                                            uint32_t maxRate = 0);

// The DoP carrier rate for a DSD source of `byteFrameRate` byte frames per second.
constexpr uint32_t dopCarrierRate(uint32_t byteFrameRate) {
    return byteFrameRate / 2;
}

} // namespace camel::audio
