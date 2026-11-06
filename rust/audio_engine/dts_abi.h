// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C ABI of rust/dts_core (docs/Plan.md §3.1). Keep in sync with rust/audio_engine/src/dts_abi.rs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DTS_OK = 0, DTS_E_NO_SYNC = -1, DTS_E_TRUNCATED = -2, DTS_E_UNSUPPORTED = -3, DTS_E_DECODE = -4, DTS_E_ARG = -5 };

typedef struct DtsDecoder DtsDecoder;
typedef struct DtsFrameInfo {
    uint32_t container_bytes; // what the frame takes in its packing
    uint32_t sample_rate;
} DtsFrameInfo;
typedef struct DtsPcm {
    const float* samples; // interleaved L R C LFE Ls Rs, of those present; valid until the decoder's next call
    uint32_t channels;
    uint32_t frames;
    uint32_t sample_rate;
} DtsPcm;

DtsDecoder* dts_new(void);
void dts_free(DtsDecoder* decoder);
void dts_reset(DtsDecoder* decoder);                                   // forgets the filters, as before a seek
int64_t dts_find_sync(const uint8_t* bytes, size_t len, size_t start); // the next frame's offset, or -1
int32_t dts_frame_info(const uint8_t* bytes, size_t len, DtsFrameInfo* info);
int32_t dts_decode(DtsDecoder* decoder, const uint8_t* bytes, size_t len, DtsPcm* pcm);

#ifdef __cplusplus
}
#endif
