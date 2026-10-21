// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <cstdint>
#include <string_view>

#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

// `pcmBits` sets the depth lossy streams decode to; it must outlive every source the host opens.
CamelDecoderHostV1 makeDecoderHostV1(const PcmBitsByFormat* pcmBits = nullptr);

// The C ABI decoder number for a decoder in decoderRegistry(): built-ins keep CAMEL_DECODER_FFMPEG..DSD_NEXUS, others
// are CAMEL_DECODER_REGISTERED_BASE + their registration index. 0 for an unknown ID.
uint32_t abiDecoder(std::string_view decoder);

} // namespace camel::audio
