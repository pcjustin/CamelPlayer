// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A zip CamelPlayer reads: an impulse-response archive the user picks, or a release it downloaded.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "Common/util/Zip.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        camel::readZip(std::string(reinterpret_cast<const char*>(data), size));
    } catch (const std::runtime_error&) {
        // what a damaged archive is meant to give
    }
    return 0;
}
