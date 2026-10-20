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

struct OutputPlan {
    DeviceFormat format;
    bool duplicate = false; // mono to both channels
    bool downmix = false;   // ITU-R BS.775 to stereo
    bool resample = false;
    bool widen = false;  // integers to float32
    bool reduce = false; // fewer bits than the source, with dither
    DsdPath dsd = DsdPath::None;
    bool shared = false; // through the WASAPI shared-mode mixer
    PipelineState state = PipelineState::BitPerfect;
    bool operator==(const OutputPlan&) const = default;
};

// Containers tried for a source of `bits` bits, smallest first: 16 -> 24 -> 24-in-32 -> 32.
std::vector<SampleType> bitPerfectCandidates(uint32_t bits);

// Rates are tried in order: the source rate, then with `convert` the lowest same-family rate at or above it, the
// lowest other-family rate at or above it, and the highest rate. Each rate tries containers that carry the source
// depth, then float32, then with `convert` a smaller depth. Output is always stereo: mono is duplicated, more channels
// need `convert`. `fixedType` (from settings) restricts the container. `lossy` marks a float decode (AAC/MP3). A
// nonzero `fixedRate` is the only rate tried, resampled without `convert`. Returns nullopt when nothing fits; the
// caller then skips the track and tells the user.
std::optional<OutputPlan> negotiateOutput(const SourceInfo& src, bool lossy, std::optional<SampleType> fixedType, bool convert,
                                          const std::function<bool(const DeviceFormat&)>& deviceSupports, uint32_t fixedRate = 0);

// Raw DSD (`src` from the DSD Nexus decoder: byte frames, 1 bit per sample), docs/Plan.md §5.3 item 4: native DSD
// when `native` is requested and the device takes it, else DoP at DSD rate / 16 in a 24-bit, 24-in-32 or 32-bit
// carrier (`fixedType` restricts it). Never PCM; mono is duplicated, more channels are refused. A carrier above
// 768 kHz (DSD512 DoP needs 1411.2 kHz) is used only if the device reports it.
std::optional<OutputPlan> negotiateDsd(const SourceInfo& src, bool native, std::optional<SampleType> fixedType,
                                       const std::function<bool(const DeviceFormat&)>& deviceSupports);

// DSD converted to PCM: decimated to 352.8 kHz, then delivered like a 24-bit source at `rate` (the setting), with
// `convert` governing other rates, downmix and depth exactly as for PCM. Always Processed; nullopt when nothing fits.
std::optional<OutputPlan> negotiateDsdToPcm(const SourceInfo& src, uint32_t rate, std::optional<SampleType> fixedType,
                                            bool convert, const std::function<bool(const DeviceFormat&)>& deviceSupports);

// The DoP carrier rate for a DSD source of `byteFrameRate` byte frames per second.
constexpr uint32_t dopCarrierRate(uint32_t byteFrameRate) {
    return byteFrameRate / 2;
}

} // namespace camel::audio
