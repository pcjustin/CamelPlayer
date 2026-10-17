#include <gtest/gtest.h>

#include <cstddef>

#include "Engine/EngineAbi.h"

static_assert(sizeof(CamelAudioFormatV1) == 40);
static_assert(sizeof(CamelEngineConfigV1) == 56);
static_assert(sizeof(CamelCommandStampV1) == 32);
static_assert(sizeof(CamelTrackSourceV1) == 48);
static_assert(sizeof(CamelSourceInfoV1) == 56);
static_assert(sizeof(CamelDecoderHostV1) == 48);
static_assert(sizeof(CamelRenderRequestV1) == 32);
static_assert(sizeof(CamelRenderResultV1) == 32);
static_assert(sizeof(CamelEngineStateV1) == 152);
static_assert(sizeof(CamelEngineTimelineV1) == 88);
static_assert(sizeof(CamelFlacIntegrityV1) == 32);
static_assert(offsetof(CamelAudioFormatV1, channel_mask) == 32);
static_assert(offsetof(CamelEngineConfigV1, format) == 16);
static_assert(offsetof(CamelCommandStampV1, command_id) == 8);
static_assert(offsetof(CamelTrackSourceV1, start_frame) == 24);
static_assert(offsetof(CamelRenderRequestV1, output_capacity_bytes) == 24);
static_assert(offsetof(CamelEngineStateV1, source_format) == 72);
static_assert(offsetof(CamelFlacIntegrityV1, total_frames) == 16);

TEST(EngineAbi, StableV1Constants) {
    EXPECT_EQ(CAMEL_ENGINE_ABI_VERSION, 1u);
    EXPECT_EQ(CAMEL_ENGINE_MAX_RENDER_UNITS, 65536u);
    EXPECT_EQ(CAMEL_ENGINE_E_ABI_MISMATCH, -2);
    EXPECT_EQ(CAMEL_ENGINE_E_BUFFER_TOO_SMALL, -4);
    EXPECT_EQ(CAMEL_ENGINE_E_SOURCE_IO, -10);
    EXPECT_EQ(CAMEL_DECODER_SNDFILE, 4u);
    EXPECT_EQ(CAMEL_DECODER_DR_WAV, 5u);
    EXPECT_EQ(CAMEL_FLAC_INTEGRITY_UNAVAILABLE, 0u);
    EXPECT_EQ(CAMEL_FLAC_INTEGRITY_FAILED, 3u);
    EXPECT_EQ(CAMEL_RENDER_STATE_UNDERRUN, 4u);
    EXPECT_EQ(CAMEL_PLAYBACK_READY, 1u);
    EXPECT_EQ(CAMEL_PLAYBACK_PAUSED, 3u);
}
