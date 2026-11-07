// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The format probe, which reads every file before any decoder does.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "Engine/decode/FormatProbe.h"
#include "OutputBridge/IOutput.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const auto path = std::filesystem::temp_directory_path() / "camelplayer-probe-fuzz.bin";
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
