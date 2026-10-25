// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/DeviceMatch.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace camel::server {
namespace {

std::string hardwareName(Backend backend, const std::string& name) {
    if (backend == Backend::Wasapi) {
        // "Speakers (Chord Async USB 44.1kHz-768kHz)"; the device name may hold parentheses of its own.
        const auto open = name.find('(');
        const auto close = name.rfind(')');
        if (open != std::string::npos && close != std::string::npos && close > open)
            return name.substr(open + 1, close - open - 1);
        return name;
    }
    if (backend == Backend::Ks) return name.substr(0, name.find(" / "));
    return name;
}

// Words that name a product rather than a kind of device, connection or company form.
std::set<std::string> distinctiveWords(const std::string& name) {
    static const std::set<std::string> generic = {"asio",        "audio",  "usb",      "async",   "driver",
                                                  "device",      "output", "speakers", "digital", "streaming",
                                                  "electronics", "ltd",    "inc",      "corp",    "the"};
    std::set<std::string> words;
    std::string word;
    bool digits = false;
    const auto flush = [&] {
        if (word.size() >= 3 && !digits && !generic.contains(word)) words.insert(word);
        word.clear();
        digits = false;
    };
    for (const unsigned char c : name) {
        if (std::isalnum(c)) {
            word += char(std::tolower(c));
            digits = digits || std::isdigit(c);
        } else {
            flush();
        }
    }
    flush();
    return words;
}

} // namespace

std::optional<std::string> matchDevice(Backend from, const DeviceInfo& device, Backend to,
                                       const std::vector<DeviceInfo>& candidates) {
    const std::string wanted = hardwareName(from, device.name);
    if (wanted.empty()) return std::nullopt;
    std::vector<const DeviceInfo*> same;
    for (const auto& candidate : candidates)
        if (hardwareName(to, candidate.name) == wanted) same.push_back(&candidate);
    if (same.size() == 1) return same.front()->id;
    if (same.size() > 1) return std::nullopt;

    const auto words = distinctiveWords(wanted);
    size_t best = 0;
    const DeviceInfo* chosen = nullptr;
    bool tie = false;
    for (const auto& candidate : candidates) {
        const auto theirs = distinctiveWords(hardwareName(to, candidate.name));
        const size_t shared =
            size_t(std::count_if(words.begin(), words.end(), [&](const auto& w) { return theirs.contains(w); }));
        if (shared > best) {
            best = shared;
            chosen = &candidate;
            tie = false;
        } else if (shared == best && shared > 0) {
            tie = true;
        }
    }
    if (!chosen || tie) return std::nullopt;
    return chosen->id;
}

} // namespace camel::server
