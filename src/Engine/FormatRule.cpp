#include "Engine/FormatRule.h"

#include <algorithm>
#include <span>

#include "Engine/SamplePacker.h"

namespace camel::audio {
namespace {

constexpr uint32_t kFamily44[] = {44100, 88200, 176400, 352800, 705600};
constexpr uint32_t kFamily48[] = {48000, 96000, 192000, 384000, 768000};

std::vector<uint32_t> candidateRates(uint32_t source, bool convert) {
    std::vector<uint32_t> rates{source};
    if (!convert) return rates;
    const bool family44 = source % 11025 == 0;
    const std::span<const uint32_t> same = family44 ? kFamily44 : kFamily48, other = family44 ? kFamily48 : kFamily44;
    for (const auto family : {same, other})
        for (uint32_t rate : family)
            if (rate >= source && rate != source) {
                rates.push_back(rate);
                break;
            }
    std::vector<uint32_t> all(std::begin(kFamily44), std::end(kFamily44));
    all.insert(all.end(), std::begin(kFamily48), std::end(kFamily48));
    std::sort(all.rbegin(), all.rend()); // the highest the device takes
    rates.insert(rates.end(), all.begin(), all.end());
    return rates;
}

uint32_t containerBits(SampleType t) {
    return t == SampleType::Int16 ? 16 : t == SampleType::Int24 || t == SampleType::Int24In32 ? 24 : 32;
}

} // namespace

std::vector<SampleType> bitPerfectCandidates(uint32_t bits) {
    std::vector<SampleType> out;
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32})
        if (canCarryBitPerfect(t, bits)) out.push_back(t);
    return out;
}

std::optional<OutputPlan> negotiateOutput(const SourceInfo& src, bool lossy, std::optional<SampleType> fixedType, bool convert,
                                          const std::function<bool(const DeviceFormat&)>& deviceSupports) {
    if (!src.sampleRate || !src.channels || src.channels > 8 || (src.channels > 2 && !convert) || !src.bitsPerSample ||
        src.bitsPerSample > 32)
        return std::nullopt;
    std::vector<SampleType> types = bitPerfectCandidates(src.bitsPerSample);
    if (src.bitsPerSample <= 24) types.push_back(SampleType::Float32);
    if (convert)
        for (SampleType t : {SampleType::Int24In32, SampleType::Int24, SampleType::Int16})
            if (containerBits(t) < src.bitsPerSample && std::find(types.begin(), types.end(), t) == types.end())
                types.push_back(t);
    for (uint32_t rate : candidateRates(src.sampleRate, convert))
        for (SampleType t : types) {
            if (fixedType && t != *fixedType) continue;
            const DeviceFormat f{rate, 2, t};
            if (!deviceSupports(f)) continue;
            OutputPlan plan{f};
            plan.duplicate = src.channels == 1;
            plan.downmix = src.channels > 2;
            plan.resample = rate != src.sampleRate;
            plan.widen = t == SampleType::Float32;
            plan.reduce = !plan.widen && containerBits(t) < src.bitsPerSample;
            plan.state = plan.resample || plan.downmix || plan.reduce ? PipelineState::Processed
                         : lossy                                      ? PipelineState::LossySource
                         : plan.widen                                 ? PipelineState::LosslessConversion
                                                                      : PipelineState::BitPerfect;
            return plan;
        }
    return std::nullopt;
}

} // namespace camel::audio
