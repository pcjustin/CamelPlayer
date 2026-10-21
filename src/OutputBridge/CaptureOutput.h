// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes exactly the bytes the engine hands to the device into a file (docs/Plan.md §12.1, §16.7).
#pragma once
#include <filesystem>
#include <functional>

#include "OutputBridge/IOutput.h"

namespace camel::audio {

struct CaptureOptions {
    std::filesystem::path file;
    // Which formats the simulated device accepts; default: every integer layout at any rate.
    std::function<bool(const DeviceFormat&)> accepts;
    // Pace the stream like a real device instead of running as fast as possible.
    bool realtime = false;
};

std::unique_ptr<IOutput> createCaptureOutput(CaptureOptions opts);

} // namespace camel::audio
