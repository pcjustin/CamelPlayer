// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/Equalizer.h"

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

EqAnalysis analyzeEq(const EqSettings& eq, uint32_t rateHz, std::span<const double> frequencies) {
    EqAnalysis analysis;
    const auto bands = engineBands(eq);
    analysis.leftDb.resize(frequencies.size());
    analysis.rightDb.resize(frequencies.size());
    CamelEqSummaryV1 summary{sizeof(CamelEqSummaryV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0.0};
    if (rateHz == 0 ||
        camel_eq_analyze_v1(bands.data(), uint32_t(bands.size()), rateHz, frequencies.data(), uint32_t(frequencies.size()),
                            analysis.leftDb.data(), analysis.rightDb.data(), &summary) != CAMEL_ENGINE_OK)
        return {false, 0.0, std::vector<double>(frequencies.size()), std::vector<double>(frequencies.size())};
    analysis.active = summary.active != 0;
    analysis.preampDb = summary.preamp_db;
    return analysis;
}

} // namespace camel::audio
