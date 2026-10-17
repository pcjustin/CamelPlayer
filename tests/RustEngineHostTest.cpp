#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "audio/engine/EngineAbi.h"

namespace {

CamelAudioFormatV1 makeFormat(uint32_t kind, uint32_t sample, uint32_t validBits, uint32_t containerBits) {
    return {sizeof(CamelAudioFormatV1), CAMEL_ENGINE_ABI_VERSION, kind, sample, 96000, 2, validBits, containerBits, 3};
}

uint64_t expectedBytes(const CamelAudioFormatV1& format, uint64_t units) {
    if (format.stream_kind == CAMEL_AUDIO_DSD) return units / 8 * format.channel_count;
    const uint64_t bytesPerSample = format.stream_kind == CAMEL_AUDIO_DOP ? 3 : format.container_bits / 8;
    return units * format.channel_count * bytesPerSample;
}

void expectRenderCapacityParity(const CamelAudioFormatV1& format, uint64_t units) {
    CamelEngineConfigV1 config{sizeof(CamelEngineConfigV1), CAMEL_ENGINE_ABI_VERSION, CAMEL_ENGINE_MAX_RENDER_UNITS, format};
    CamelEngine* engine = nullptr;
    ASSERT_EQ(camel_engine_create_v1(&config, &engine), CAMEL_ENGINE_OK);
    ASSERT_NE(engine, nullptr);

    std::array<uint8_t, 512> output;
    output.fill(0xA5);
    CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, units, output.data(), output.size()};
    CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, UINT64_MAX, 0, UINT32_MAX, UINT32_MAX};
    EXPECT_EQ(camel_engine_render_v1(engine, &request, &result), CAMEL_ENGINE_E_NOT_READY);
    EXPECT_EQ(result.required_bytes, expectedBytes(format, units));
    EXPECT_EQ(result.units_rendered, 0u);
    EXPECT_EQ(result.state, CAMEL_RENDER_STATE_NO_SOURCE);
    EXPECT_EQ(result.reserved, 0u);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0xA5; }));
    EXPECT_EQ(camel_engine_quiesce_v1(engine), CAMEL_ENGINE_OK);
    EXPECT_EQ(camel_engine_destroy_v1(engine), CAMEL_ENGINE_OK);
}

TEST(RustEngineHost, CAbiLinksAndReportsVersion) {
    EXPECT_EQ(camel_engine_abi_version(), CAMEL_ENGINE_ABI_VERSION);
}

TEST(RustEngineHost, PcmDopAndDsdCapacityMatchesCxxContract) {
    expectRenderCapacityParity(makeFormat(CAMEL_AUDIO_PCM, CAMEL_SAMPLE_S16_LE, 16, 16), 48);
    expectRenderCapacityParity(makeFormat(CAMEL_AUDIO_PCM, CAMEL_SAMPLE_S24_LE, 24, 24), 48);
    expectRenderCapacityParity(makeFormat(CAMEL_AUDIO_PCM, CAMEL_SAMPLE_S24_IN_32_LE, 24, 32), 48);
    expectRenderCapacityParity(makeFormat(CAMEL_AUDIO_DOP, CAMEL_SAMPLE_DOP_S24_LE, 24, 24), 48);
    expectRenderCapacityParity(makeFormat(CAMEL_AUDIO_DSD, CAMEL_SAMPLE_DSD_U8, 1, 8), 128);
}

TEST(RustEngineHost, RenderRejectsInsufficientOutputBufferWithoutWriting) {
    const auto format = makeFormat(CAMEL_AUDIO_PCM, CAMEL_SAMPLE_S24_LE, 24, 24);
    CamelEngineConfigV1 config{sizeof(CamelEngineConfigV1), CAMEL_ENGINE_ABI_VERSION, CAMEL_ENGINE_MAX_RENDER_UNITS, format};
    CamelEngine* engine = nullptr;
    ASSERT_EQ(camel_engine_create_v1(&config, &engine), CAMEL_ENGINE_OK);

    std::array<uint8_t, 8> output;
    output.fill(0x5A);
    CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, 2, output.data(), output.size() - 1};
    CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0, 0};
    EXPECT_EQ(camel_engine_render_v1(engine, &request, &result), CAMEL_ENGINE_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(result.required_bytes, 12u);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0x5A; }));
    EXPECT_EQ(camel_engine_destroy_v1(engine), CAMEL_ENGINE_OK);
}

} // namespace
