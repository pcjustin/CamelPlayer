// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/settings/EqText.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <sstream>

namespace camel {
namespace {

std::string upper(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::optional<double> number(std::string_view text) {
    double value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<EqKind> kindOf(std::string_view code) {
    if (code == "PK" || code == "PEQ") return EqKind::Peaking;
    if (code == "LS" || code == "LSC") return EqKind::LowShelf;
    if (code == "HS" || code == "HSC") return EqKind::HighShelf;
    if (code == "LP" || code == "LPQ") return EqKind::LowPass;
    if (code == "HP" || code == "HPQ") return EqKind::HighPass;
    if (code == "BP") return EqKind::BandPass;
    if (code == "NO") return EqKind::Notch;
    if (code == "AP") return EqKind::AllPass;
    return std::nullopt;
}

const char* codeOf(EqKind kind) {
    switch (kind) {
    case EqKind::LowShelf:
        return "LSC";
    case EqKind::HighShelf:
        return "HSC";
    case EqKind::LowPass:
        return "LPQ";
    case EqKind::HighPass:
        return "HPQ";
    case EqKind::BandPass:
        return "BP";
    case EqKind::Notch:
        return "NO";
    case EqKind::AllPass:
        return "AP";
    default:
        return "PK";
    }
}

} // namespace

std::vector<EqBand> parseEqualizerApo(std::string_view text) {
    std::vector<EqBand> bands;
    auto channels = EqChannels::All;
    std::istringstream lines{std::string(text)};
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(upper(line));
        std::vector<std::string> tokens;
        for (std::string word; words >> word;) tokens.push_back(word);
        if (tokens.empty()) continue;
        if (tokens[0] == "CHANNEL:" && tokens.size() >= 2) {
            channels = tokens[1] == "L" || tokens[1] == "1"   ? EqChannels::Left
                       : tokens[1] == "R" || tokens[1] == "2" ? EqChannels::Right
                                                              : EqChannels::All;
            continue;
        }
        if (!tokens[0].starts_with("FILTER")) continue;
        // "Filter 1: ON PK ..." or "Filter: ON PK ...": the state follows the token that ends with a colon.
        size_t at = 0;
        while (at < tokens.size() && !tokens[at].ends_with(':')) ++at;
        if (at + 2 >= tokens.size()) continue;
        const auto kind = kindOf(tokens[at + 2]);
        if (!kind) continue;
        EqBand band;
        band.kind = *kind;
        band.channels = channels;
        band.enabled = tokens[at + 1] == "ON";
        bool frequency = false;
        for (size_t i = at + 3; i + 1 < tokens.size(); ++i) {
            const auto value = number(tokens[i + 1]);
            if (!value) continue;
            if (tokens[i] == "FC") {
                band.frequencyHz = *value;
                frequency = true;
            } else if (tokens[i] == "GAIN") {
                band.gainDb = *value;
            } else if (tokens[i] == "Q") {
                band.q = *value;
            }
        }
        if (frequency && band.frequencyHz > 0 && band.q > 0) bands.push_back(band);
    }
    return bands;
}

std::string formatEqualizerApo(const std::vector<EqBand>& bands, double preampDb) {
    std::string text;
    char line[160];
    // Rounded down, so the written preamp never leaves less headroom than the bands need.
    snprintf(line, sizeof line, "Preamp: %.1f dB\r\n", std::floor(preampDb * 10) / 10 + 0.0);
    text += line;
    auto channels = EqChannels::All;
    int index = 0;
    for (const auto& band : bands) {
        if (band.channels != channels) {
            channels = band.channels;
            text += channels == EqChannels::Left    ? "Channel: L\r\n"
                    : channels == EqChannels::Right ? "Channel: R\r\n"
                                                    : "Channel: ALL\r\n";
        }
        const bool gain = band.kind == EqKind::Peaking || band.kind == EqKind::LowShelf || band.kind == EqKind::HighShelf;
        int length = snprintf(line, sizeof line, "Filter %d: %s %s Fc %g Hz", ++index, band.enabled ? "ON" : "OFF",
                              codeOf(band.kind), band.frequencyHz);
        if (gain) length += snprintf(line + length, sizeof line - size_t(length), " Gain %.1f dB", band.gainDb);
        snprintf(line + length, sizeof line - size_t(length), " Q %.2f\r\n", band.q);
        text += line;
    }
    return text;
}

} // namespace camel
