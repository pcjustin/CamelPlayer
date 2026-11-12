// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The folder scan-bench builds its library in.
#pragma once
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace camelcli {

// In every folder scan-bench made, so that it empties only those.
inline constexpr wchar_t kBenchMarker[] = L"camelcli-scan-bench.txt";

// `outer` is `inner` or one of the folders above it, whatever the case of its letters.
inline bool holds(const std::filesystem::path& outer, const std::filesystem::path& inner) {
    std::error_code ec;
    const auto a = std::filesystem::weakly_canonical(std::filesystem::absolute(outer, ec), ec);
    const auto b = std::filesystem::weakly_canonical(std::filesystem::absolute(inner, ec), ec);
    auto at = b.begin();
    for (const auto& part : a) {
        if (part.empty()) continue; // a trailing separator
        if (at == b.end()) return false;
        const auto x = part.wstring();
        const auto y = at->wstring();
        if (x.size() != y.size() ||
            !std::equal(x.begin(), x.end(), y.begin(), [](wchar_t c, wchar_t d) { return std::towlower(c) == std::towlower(d); }))
            return false;
        ++at;
    }
    return true;
}

// Empties `data` for a fresh library, only when it is missing, empty or one scan-bench made, and never when it is or
// holds one of `playerData`. Returns why it was left as it is, or nothing once it is ready.
inline std::optional<std::string> clearBenchData(const std::filesystem::path& data,
                                                 const std::vector<std::filesystem::path>& playerData) {
    namespace fs = std::filesystem;
    for (const auto& player : playerData)
        if (!player.empty() && holds(data, player)) return "it is, or holds, CamelPlayer's own data";
    std::error_code ec;
    if (fs::exists(data, ec)) {
        if (!fs::is_directory(data, ec)) return "it is not a folder";
        if (!fs::is_empty(data, ec) && !fs::exists(data / kBenchMarker, ec))
            return "it is not empty, and scan-bench did not make it";
        fs::remove_all(data, ec);
        if (ec) return "it cannot be emptied: " + ec.message();
    }
    fs::create_directories(data, ec);
    std::ofstream(data / kBenchMarker) << "Made by camelcli scan-bench, which empties this folder when it runs again.\n";
    if (!fs::exists(data / kBenchMarker, ec)) return "it cannot be written";
    return std::nullopt;
}

} // namespace camelcli
