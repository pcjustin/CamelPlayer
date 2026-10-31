// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/Dsp.h"

namespace camel::audio {

std::vector<CamelEqBandV1> engineBands(const EqSettings& eq) {
    std::vector<CamelEqBandV1> bands;
    if (!eq.enabled) return bands;
    for (const auto& b : eq.bands)
        if (b.enabled)
            bands.push_back({sizeof(CamelEqBandV1), CAMEL_ENGINE_ABI_VERSION, uint32_t(b.kind), uint32_t(b.channels), b.slope, 1,
                             b.frequencyHz, b.gainDb, b.q});
    return bands;
}

CamelDspV1 DspChain::abi() const {
    return {sizeof(CamelDspV1),
            CAMEL_ENGINE_ABI_VERSION,
            {order[0], order[1], order[2]},
            uint32_t(bands.size()),
            bands.data(),
            crossfeed ? &*crossfeed : nullptr,
            impulse.get()};
}

DspChain dspChain(const EqSettings& eq, const CrossfeedSettings& crossfeed, const ConvolutionSettings& convolution,
                  const DspOrder& order, Impulse impulse) {
    DspChain chain;
    for (size_t i = 0; i < chain.order.size(); ++i) chain.order[i] = uint32_t(order[i]);
    chain.bands = engineBands(eq);
    if (crossfeed.enabled) {
        const auto [cutoff, feed] = crossfeedValues(crossfeed);
        chain.crossfeed = CamelCrossfeedV1{sizeof(CamelCrossfeedV1), CAMEL_ENGINE_ABI_VERSION, cutoff, 0, feed};
    }
    if (convolution.enabled) chain.impulse = std::move(impulse);
    return chain;
}

DspChain dspChain(const Settings& settings, Impulse impulse) {
    return dspChain(settings.eq, settings.crossfeed, settings.convolution, settings.dspOrder, std::move(impulse));
}

DspChain eqChain(const EqSettings& eq) {
    return dspChain(eq, {}, {}, kDefaultDspOrder, nullptr);
}

DspAnalysis analyzeDsp(const DspChain& chain, uint32_t rateHz, std::span<const double> frequencies) {
    DspAnalysis analysis;
    analysis.leftDb.resize(frequencies.size());
    analysis.rightDb.resize(frequencies.size());
    const auto abi = chain.abi();
    CamelDspSummaryV1 summary{sizeof(CamelDspSummaryV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0.0};
    if (rateHz == 0 || camel_dsp_analyze_v1(&abi, rateHz, frequencies.data(), uint32_t(frequencies.size()),
                                            analysis.leftDb.data(), analysis.rightDb.data(), &summary) != CAMEL_ENGINE_OK)
        return {false, 0.0, std::vector<double>(frequencies.size()), std::vector<double>(frequencies.size())};
    analysis.active = summary.active != 0;
    analysis.preampDb = summary.preamp_db;
    return analysis;
}

std::vector<DspNode> actingNodes(const DspChain& chain, uint32_t rateHz, uint32_t channels) {
    std::vector<DspNode> nodes;
    for (const auto id : chain.order) {
        const auto node = DspNode(id);
        bool acts = false;
        if (node == DspNode::Eq) {
            DspChain eq;
            eq.order = chain.order;
            eq.bands = chain.bands;
            acts = analyzeDsp(eq, rateHz).active;
        } else if (node == DspNode::Crossfeed) {
            acts = chain.crossfeed.has_value() && channels >= 2;
        } else {
            acts = chain.impulse != nullptr;
        }
        if (acts) nodes.push_back(node);
    }
    return nodes;
}

} // namespace camel::audio
