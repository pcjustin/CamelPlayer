// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Common/settings/Settings.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

// `pcmBits` sets the depth lossy streams decode to; it must outlive every source the host opens.
CamelDecoderHostV1 makeDecoderHostV1(const PcmBitsByFormat* pcmBits = nullptr);

} // namespace camel::audio
