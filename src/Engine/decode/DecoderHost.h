// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "Engine/EngineAbi.h"
#include "Engine/decode/IDecoder.h"

namespace camel::audio {

// A decoder already open on `path`, which the host's first open of `path` with decoder number `kind` takes rather than
// opening the file again.
struct OpenedDecoder {
    std::unique_ptr<IDecoder> decoder;
    std::filesystem::path path;
    uint32_t kind = 0;
};

// `opened`, when given, outlives the host.
CamelDecoderHostV1 makeDecoderHostV1(OpenedDecoder* opened = nullptr);

// The C ABI decoder number for a decoder in decoderRegistry(): built-ins keep CAMEL_DECODER_SYMPHONIA..DSD_NEXUS, others
// are CAMEL_DECODER_REGISTERED_BASE + their registration index. 0 for an unknown ID.
uint32_t abiDecoder(std::string_view decoder);

} // namespace camel::audio
