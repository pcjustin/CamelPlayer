// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <stddef.h>

#include "Engine/EngineAbi.h"

typedef char CamelAudioFormatV1Size[(sizeof(CamelAudioFormatV1) == 40) ? 1 : -1];
typedef char CamelEngineConfigV1Size[(sizeof(CamelEngineConfigV1) == 72) ? 1 : -1];
typedef char CamelCommandStampV1Size[(sizeof(CamelCommandStampV1) == 32) ? 1 : -1];
typedef char CamelTrackSourceV1Size[(sizeof(CamelTrackSourceV1) == 48) ? 1 : -1];
typedef char CamelSourceInfoV1Size[(sizeof(CamelSourceInfoV1) == 56) ? 1 : -1];
typedef char CamelDecoderHostV1Size[(sizeof(CamelDecoderHostV1) == 48) ? 1 : -1];
typedef char CamelRenderRequestV1Size[(sizeof(CamelRenderRequestV1) == 32) ? 1 : -1];
typedef char CamelRenderResultV1Size[(sizeof(CamelRenderResultV1) == 32) ? 1 : -1];
typedef char CamelEngineStateV1Size[(sizeof(CamelEngineStateV1) == 152) ? 1 : -1];
typedef char CamelFlacIntegrityV1Size[(sizeof(CamelFlacIntegrityV1) == 32) ? 1 : -1];

uint32_t camel_engine_abi_header_c_compiles(void) {
    return CAMEL_ENGINE_ABI_VERSION;
}
