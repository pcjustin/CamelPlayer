#include "audio/engine/FormatRule.h"

#include "audio/engine/SamplePacker.h"

namespace camel::audio {

std::vector<SampleType> bitPerfectCandidates(uint32_t bits) {
    std::vector<SampleType> out;
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32})
        if (canCarryBitPerfect(t, bits)) out.push_back(t);
    return out;
}

std::optional<DeviceFormat> chooseOutputFormat(const SourceInfo& src, std::optional<SampleType> fixedType,
                                               const std::function<bool(const DeviceFormat&)>& deviceSupports) {
    if (!src.sampleRate || src.channels != 2) return std::nullopt; // MVP: stereo only (downmix arrives in M6)
    for (SampleType t : bitPerfectCandidates(src.bitsPerSample)) {
        if (fixedType && t != *fixedType) continue;
        DeviceFormat f{src.sampleRate, src.channels, t};
        if (deviceSupports(f)) return f;
    }
    return std::nullopt;
}

} // namespace camel::audio
