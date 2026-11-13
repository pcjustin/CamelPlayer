// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The format probe, which reads every file before any decoder does.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <process.h>

#include "Engine/decode/FormatProbe.h"
#include "OutputBridge/IOutput.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // One file a process: -jobs, -workers and -fork run several, which would write over each other's input.
    static const auto path =
        std::filesystem::temp_directory_path() / ("camelplayer-probe-fuzz-" + std::to_string(_getpid()) + ".bin");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    }
    try {
        camel::audio::probeFormat(path);
    } catch (const camel::audio::Error&) {
        // what a damaged file is meant to give
    }
    return 0;
}
