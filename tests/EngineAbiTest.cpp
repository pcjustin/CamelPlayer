#include <gtest/gtest.h>

#include <cstddef>

#include "audio/engine/EngineAbi.h"

static_assert(sizeof(CamelAudioFormatV1) == 40);
static_assert(sizeof(CamelEngineConfigV1) == 56);
static_assert(sizeof(CamelCommandStampV1) == 32);
static_assert(sizeof(CamelTrackSourceV1) == 48);
static_assert(sizeof(CamelRenderRequestV1) == 32);
static_assert(sizeof(CamelRenderResultV1) == 32);
static_assert(sizeof(CamelEngineStateV1) == 152);
static_assert(offsetof(CamelAudioFormatV1, channel_mask) == 32);
static_assert(offsetof(CamelEngineConfigV1, format) == 16);
static_assert(offsetof(CamelCommandStampV1, command_id) == 8);
static_assert(offsetof(CamelTrackSourceV1, start_frame) == 24);
static_assert(offsetof(CamelRenderRequestV1, output_capacity_bytes) == 24);
static_assert(offsetof(CamelEngineStateV1, source_format) == 72);

TEST(EngineAbi, StableV1Constants) {
    EXPECT_EQ(CAMEL_ENGINE_ABI_VERSION, 1u);
    EXPECT_EQ(CAMEL_ENGINE_MAX_RENDER_UNITS, 65536u);
    EXPECT_EQ(CAMEL_ENGINE_E_ABI_MISMATCH, -2);
    EXPECT_EQ(CAMEL_ENGINE_E_BUFFER_TOO_SMALL, -4);
}
