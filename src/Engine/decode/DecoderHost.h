#pragma once

#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

// `pcmBits` sets the depth lossy streams decode to; it must outlive every source the host opens.
CamelDecoderHostV1 makeDecoderHostV1(const PcmBitsByFormat* pcmBits = nullptr);

} // namespace camel::audio
