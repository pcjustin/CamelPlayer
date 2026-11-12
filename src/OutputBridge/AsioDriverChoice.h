// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Which ASIO driver an output loads: the saved one, never replaced by another, or with none saved the first one listed as
// available.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "Common/AudioTypes.h"
#include "OutputBridge/IOutput.h"

namespace camel::audio {

inline std::string asioDriverToLoad(const std::vector<DeviceEntry>& drivers, const std::string& saved) {
    const auto available = [](const DeviceEntry& d) { return d.name.find("(unavailable)") == std::string::npos; };
    if (saved.empty()) {
        const auto first = std::find_if(drivers.begin(), drivers.end(), available);
        if (first == drivers.end()) throw Error(ErrorCode::DeviceNotFound, "no ASIO driver installed");
        return first->id;
    }
    if (std::none_of(drivers.begin(), drivers.end(), [&](const DeviceEntry& d) { return d.id == saved && available(d); }))
        throw Error(ErrorCode::DeviceNotFound, "ASIO driver not connected (not installed)");
    return saved;
}

} // namespace camel::audio
