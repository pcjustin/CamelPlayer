// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/FormatRule.h"

#include <algorithm>
#include <iterator>
#include <span>

#include "Engine/SamplePacker.h"

namespace camel::audio {
namespace {

constexpr uint32_t kFamily44[] = {44100, 88200, 176400, 352800, 705600};
constexpr uint32_t kFamily48[] = {48000, 96000, 192000, 384000, 768000};
// Rates a shared endpoint can mix at below both families, tried last.
constexpr uint32_t kLowRates[] = {32000, 24000, 22050, 16000, 12000, 11025, 8000};

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
    rates.insert(rates.end(), std::begin(kLowRates), std::end(kLowRates));
    return rates;
}

constexpr uint32_t kDsd64ByteRate = 2822400 / 8;

uint32_t containerBits(SampleType t) {
    return t == SampleType::Int16 ? 16 : t == SampleType::Int24 || t == SampleType::Int24In32 ? 24 : 32;
}

} // namespace

uint32_t limitedRate(uint32_t rate, uint32_t maxRate) {
    if (!maxRate || rate <= maxRate) return rate;
    const auto highest = [&](std::span<const uint32_t> family) {
        uint32_t best = 0;
        for (uint32_t r : family)
            if (r <= maxRate) best = r;
        return best;
    };
    const bool family44 = rate % 11025 == 0;
    if (const uint32_t same = highest(family44 ? kFamily44 : kFamily48)) return same;
    if (const uint32_t other = highest(family44 ? kFamily48 : kFamily44)) return other;
    return maxRate;
}

std::vector<SampleType> bitPerfectCandidates(uint32_t bits) {
    std::vector<SampleType> out;
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32})
        if (canCarryBitPerfect(t, bits)) out.push_back(t);
    return out;
}

std::optional<OutputPlan> negotiateOutput(const SourceInfo& src, bool lossy, std::optional<SampleType> fixedType, bool convert,
                                          const std::function<bool(const DeviceFormat&)>& deviceSupports, uint32_t fixedRate,
                                          uint32_t maxRate) {
    if (!src.sampleRate || !src.channels || src.channels > 8 || (src.channels > 2 && !convert) || !src.bitsPerSample ||
        src.bitsPerSample > 32)
        return std::nullopt;
    std::vector<SampleType> types = bitPerfectCandidates(src.bitsPerSample);
    if (src.bitsPerSample <= 24) types.push_back(SampleType::Float32);
    if (convert)
        for (SampleType t : {SampleType::Int24In32, SampleType::Int24, SampleType::Int16})
            if (containerBits(t) < src.bitsPerSample && std::find(types.begin(), types.end(), t) == types.end())
                types.push_back(t);
    std::vector<uint32_t> rates;
    if (fixedRate) {
        rates = {fixedRate};
    } else if (maxRate && src.sampleRate > maxRate) {
        rates = {limitedRate(src.sampleRate, maxRate)};
        if (convert) std::ranges::copy(candidateRates(src.sampleRate, true), std::back_inserter(rates));
    } else {
        rates = candidateRates(src.sampleRate, convert);
    }
    for (uint32_t rate : rates)
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

std::optional<OutputPlan> negotiateDsd(const SourceInfo& src, bool native, std::optional<SampleType> fixedType,
                                       const std::function<bool(const DeviceFormat&)>& deviceSupports, uint32_t maxByteRate,
                                       uint32_t maxPcmRate) {
    if (src.bitsPerSample != 1 || !src.sampleRate || src.sampleRate % kDsd64ByteRate || !src.channels || src.channels > 2)
        return std::nullopt;
    const auto at = [&](uint32_t rate) -> std::optional<OutputPlan> {
        OutputPlan plan;
        plan.duplicate = src.channels == 1;
        if (rate != src.sampleRate) {
            plan.remodulate = true;
            plan.state = PipelineState::Processed;
        }
        if (native && deviceSupports({rate, 2, SampleType::Dsd8})) {
            plan.format = {rate, 2, SampleType::Dsd8};
            plan.dsd = DsdPath::Native;
            return plan;
        }
        for (SampleType t : {SampleType::Int24, SampleType::Int24In32, SampleType::Int32}) {
            const DeviceFormat f{dopCarrierRate(rate), 2, t};
            if ((fixedType && t != *fixedType) || !deviceSupports(f)) continue;
            plan.format = f;
            plan.dsd = DsdPath::Dop;
            return plan;
        }
        return std::nullopt;
    };
    uint32_t rate = src.sampleRate;
    while (maxByteRate && rate > maxByteRate && rate > kDsd64ByteRate) rate /= 2;
    for (;;) {
        if (auto plan = at(rate)) return plan;
        if (!maxPcmRate || dopCarrierRate(rate) <= maxPcmRate || rate <= kDsd64ByteRate) return std::nullopt;
        rate /= 2;
    }
}

std::optional<OutputPlan> negotiateDsdToPcm(const SourceInfo& src, uint32_t rate, std::optional<SampleType> fixedType,
                                            bool convert, const std::function<bool(const DeviceFormat&)>& deviceSupports,
                                            uint32_t maxRate) {
    if (src.bitsPerSample != 1) return std::nullopt;
    auto plan = negotiateOutput({rate, src.channels, 24}, false, fixedType, convert, deviceSupports, 0, maxRate);
    if (!plan) return std::nullopt;
    plan->dsd = DsdPath::Pcm;
    plan->resample = plan->format.sampleRate != 352800; // from the decimator's 352.8 kHz
    plan->state = PipelineState::Processed;
    return plan;
}

} // namespace camel::audio
