#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "Engine/decode/DecoderHost.h"
#include "Engine/testing/TestAudio.h"
#include "Engine/EngineAbi.h"
#include "Engine/SamplePacker.h"
#include "Common/util/Text.h"
#include "TestUtil.h"

namespace {

CamelAudioFormatV1 makeFormat(uint32_t kind, uint32_t sample, uint32_t validBits, uint32_t containerBits) {
    return {sizeof(CamelAudioFormatV1), CAMEL_ENGINE_ABI_VERSION, kind, sample, 96000, 2, validBits, containerBits, 3};
}

uint64_t expectedBytes(const CamelAudioFormatV1& format, uint64_t units) {
    if (format.stream_kind == CAMEL_AUDIO_DSD) return units / 8 * format.channel_count;
    const uint64_t bytesPerSample = format.stream_kind == CAMEL_AUDIO_DOP ? 3 : format.container_bits / 8;
    return units * format.channel_count * bytesPerSample;
}

CamelEngine* createEngine() {
    const auto format = makeFormat(CAMEL_AUDIO_PCM, CAMEL_SAMPLE_S24_LE, 24, 24);
    CamelEngineConfigV1 config{sizeof(CamelEngineConfigV1), CAMEL_ENGINE_ABI_VERSION, CAMEL_ENGINE_MAX_RENDER_UNITS, format};
    CamelEngine* engine = nullptr;
    EXPECT_EQ(camel_engine_create_v1(&config, &engine), CAMEL_ENGINE_OK);
    return engine;
}

int32_t loadSource(CamelEngine* engine, const std::filesystem::path& path, uint32_t decoderKind, uint64_t start = 0,
                   uint64_t end = 0, bool hasEnd = false) {
    const auto utf8Path = camel::toUtf8(path.wstring());
    CamelCommandStampV1 command{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 1, 1, 1};
    CamelTrackSourceV1 source{sizeof(CamelTrackSourceV1),
                              CAMEL_ENGINE_ABI_VERSION,
                              reinterpret_cast<const uint8_t*>(utf8Path.data()),
                              utf8Path.size(),
                              start,
                              end,
                              hasEnd ? 1u : 0u,
                              0};
    const auto host = camel::audio::makeDecoderHostV1();
    return camel_engine_load_flac_v1(engine, &command, &source, decoderKind, &host);
}

CamelFlacIntegrityV1 flacIntegrity(CamelEngine* engine) {
    CamelFlacIntegrityV1 integrity{
        sizeof(CamelFlacIntegrityV1), CAMEL_ENGINE_ABI_VERSION, UINT32_MAX, UINT32_MAX, UINT64_MAX, UINT64_MAX};
    EXPECT_EQ(camel_engine_get_flac_integrity_v1(engine, &integrity), CAMEL_ENGINE_OK);
    return integrity;
}

void destroyEngine(CamelEngine* engine) {
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(camel_engine_quiesce_v1(engine), CAMEL_ENGINE_OK);
    EXPECT_EQ(camel_engine_destroy_v1(engine), CAMEL_ENGINE_OK);
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

TEST(RustEngineHost, PrefillAndRenderPackPcmAndReportStarvation) {
    const auto verifyFormat = [](const char* name, uint32_t bits) {
        const auto path = testutil::testFile(name);
        const auto reference = camel::audio::testaudio::referenceDecode(path);
        ASSERT_TRUE(reference.error.empty()) << reference.error;
        auto* engine = createEngine();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(loadSource(engine, path, CAMEL_DECODER_SNDFILE), CAMEL_ENGINE_OK);
        const CamelCommandStampV1 play{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 2, 2, 1};
        ASSERT_EQ(camel_engine_play_v1(engine, &play), CAMEL_ENGINE_OK);

        constexpr uint64_t frames = 256;
        uint64_t framesPrefilled = 0;
        ASSERT_EQ(camel_engine_prefill_v1(engine, frames, &framesPrefilled), CAMEL_ENGINE_OK);
        ASSERT_EQ(framesPrefilled, frames);
        std::array<uint8_t, frames * 2 * 3> output{};
        CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, frames, output.data(),
                                     output.size()};
        CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0, 0};
        EXPECT_EQ(camel_engine_render_v1(engine, &request, &result), CAMEL_ENGINE_OK);
        EXPECT_EQ(result.units_rendered, frames);
        EXPECT_EQ(result.state, CAMEL_RENDER_STATE_ACTIVE);

        std::array<uint8_t, frames * 2 * 3> expected{};
        camel::audio::packSamples(reference.samples.data(), frames * 2, bits, camel::audio::SampleType::Int24, expected.data());
        EXPECT_EQ(output, expected);

        std::array<uint8_t, 24> silence;
        silence.fill(0xA5);
        request.units = 4;
        request.output = silence.data();
        request.output_capacity_bytes = silence.size();
        EXPECT_EQ(camel_engine_render_v1(engine, &request, &result), CAMEL_ENGINE_UNDERRUN);
        EXPECT_EQ(result.units_rendered, 0u);
        EXPECT_EQ(result.state, CAMEL_RENDER_STATE_UNDERRUN);
        EXPECT_TRUE(std::all_of(silence.begin(), silence.end(), [](uint8_t value) { return value == 0; }));
        destroyEngine(engine);
    };

    verifyFormat("16-44.1k", 16);
    verifyFormat("24-96k", 24);
}

TEST(RustEngineHost, PlaybackCommandsExposeSeparateTimelinePositions) {
    auto* engine = createEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(loadSource(engine, testutil::testFile("16-44.1k"), CAMEL_DECODER_SNDFILE), CAMEL_ENGINE_OK);

    CamelCommandStampV1 play{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 2, 2, 1};
    ASSERT_EQ(camel_engine_play_v1(engine, &play), CAMEL_ENGINE_OK);
    uint64_t prefilled = 0;
    ASSERT_EQ(camel_engine_prefill_v1(engine, 4, &prefilled), CAMEL_ENGINE_OK);
    ASSERT_EQ(prefilled, 4u);

    std::array<uint8_t, 12> output{};
    CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, 2, output.data(), output.size()};
    CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0, 0};
    ASSERT_EQ(camel_engine_render_v1(engine, &request, &result), CAMEL_ENGINE_OK);
    ASSERT_EQ(camel_engine_report_audible_position_v1(engine, &play, 1), CAMEL_ENGINE_OK);

    CamelEngineTimelineV1 timeline{sizeof(CamelEngineTimelineV1), CAMEL_ENGINE_ABI_VERSION};
    ASSERT_EQ(camel_engine_get_timeline_v1(engine, &timeline), CAMEL_ENGINE_OK);
    EXPECT_EQ(timeline.playback_state, CAMEL_PLAYBACK_PLAYING);
    EXPECT_EQ(timeline.decoded_position_frames, 4u);
    EXPECT_EQ(timeline.submitted_position_frames, 2u);
    EXPECT_EQ(timeline.audible_position_frames, 1u);

    CamelCommandStampV1 pause{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 3, 3, 1};
    ASSERT_EQ(camel_engine_pause_v1(engine, &pause), CAMEL_ENGINE_OK);
    EXPECT_EQ(camel_engine_report_audible_position_v1(engine, &play, 2), CAMEL_ENGINE_E_NOT_READY);
    ASSERT_EQ(camel_engine_get_timeline_v1(engine, &timeline), CAMEL_ENGINE_OK);
    EXPECT_EQ(timeline.playback_state, CAMEL_PLAYBACK_PAUSED);
    EXPECT_EQ(timeline.audible_position_frames, 1u);

    CamelCommandStampV1 stop{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 4, 4, 1};
    ASSERT_EQ(camel_engine_stop_v1(engine, &stop), CAMEL_ENGINE_OK);
    ASSERT_EQ(camel_engine_get_timeline_v1(engine, &timeline), CAMEL_ENGINE_OK);
    EXPECT_EQ(timeline.playback_state, CAMEL_PLAYBACK_STOPPED);
    EXPECT_EQ(timeline.decoded_position_frames, 0u);
    EXPECT_EQ(timeline.submitted_position_frames, 0u);
    EXPECT_EQ(timeline.audible_position_frames, 0u);
    destroyEngine(engine);
}

TEST(RustEngineHost, AllFlacDecodersMatchIndependentReferenceAndSeekExactly) {
    constexpr std::array<uint32_t, 4> decoders = {CAMEL_DECODER_FFMPEG, CAMEL_DECODER_SYMPHONIA, CAMEL_DECODER_DR_FLAC,
                                                  CAMEL_DECODER_SNDFILE};
    for (const auto* name : {"16-44.1k", "24-96k"}) {
        const auto path = testutil::testFile(name);
        const auto reference = camel::audio::testaudio::referenceDecode(path);
        ASSERT_TRUE(reference.error.empty()) << reference.error;
        for (const auto decoderKind : decoders) {
            auto* engine = createEngine();
            ASSERT_NE(engine, nullptr);
            ASSERT_EQ(loadSource(engine, path, decoderKind), CAMEL_ENGINE_OK) << decoderKind << " " << name;

            CamelEngineStateV1 state{};
            state.struct_size = sizeof(state);
            state.abi_version = CAMEL_ENGINE_ABI_VERSION;
            ASSERT_EQ(camel_engine_get_state_v1(engine, &state), CAMEL_ENGINE_OK);
            EXPECT_EQ(state.source_format.rate_hz, reference.info.sampleRate);
            EXPECT_EQ(state.source_format.channel_count, reference.info.channels);
            EXPECT_EQ(state.source_format.valid_bits, reference.info.bitsPerSample);
            EXPECT_EQ(state.total_frames, reference.info.totalFrames);
            EXPECT_EQ(flacIntegrity(engine).status, CAMEL_FLAC_INTEGRITY_INCOMPLETE);

            std::vector<int32_t> samples;
            std::array<int32_t, 4096 * 2> chunk{};
            for (;;) {
                uint64_t framesRead = 0;
                const auto status = camel_engine_read_decoded_v1(engine, chunk.data(), 4096, &framesRead);
                ASSERT_TRUE(status == CAMEL_ENGINE_OK || status == CAMEL_ENGINE_END) << status;
                samples.insert(samples.end(), chunk.begin(), chunk.begin() + framesRead * reference.info.channels);
                if (status == CAMEL_ENGINE_END) break;
            }
            EXPECT_EQ(samples, reference.samples) << "decoder=" << decoderKind << " file=" << name;
            ASSERT_EQ(camel_engine_get_state_v1(engine, &state), CAMEL_ENGINE_OK);
            EXPECT_EQ(state.position_frames, reference.info.totalFrames);
            const auto verified = flacIntegrity(engine);
            EXPECT_EQ(verified.status, CAMEL_FLAC_INTEGRITY_VERIFIED) << "decoder=" << decoderKind << " file=" << name;
            EXPECT_EQ(verified.has_reference_md5, 1u);
            EXPECT_EQ(verified.total_frames, reference.info.totalFrames);
            EXPECT_EQ(verified.position_frames, reference.info.totalFrames);

            constexpr uint64_t seekFrame = 12345;
            CamelCommandStampV1 command{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 2, 1, 1};
            ASSERT_EQ(camel_engine_seek_v1(engine, &command, seekFrame), CAMEL_ENGINE_OK);
            std::array<int32_t, 512> seekSamples{};
            uint64_t framesRead = 0;
            ASSERT_EQ(camel_engine_read_decoded_v1(engine, seekSamples.data(), 256, &framesRead), CAMEL_ENGINE_OK);
            ASSERT_EQ(framesRead, 256u);
            const auto begin = reference.samples.begin() + seekFrame * reference.info.channels;
            EXPECT_TRUE(std::equal(seekSamples.begin(), seekSamples.end(), begin))
                << "seek decoder=" << decoderKind << " file=" << name;
            ASSERT_EQ(camel_engine_get_state_v1(engine, &state), CAMEL_ENGINE_OK);
            EXPECT_EQ(state.position_frames, seekFrame + framesRead);
            destroyEngine(engine);
        }
    }
}

TEST(RustEngineHost, ReportsIncompleteUnavailableAndFailedFlacIntegrity) {
    constexpr std::array<uint32_t, 4> decoders = {CAMEL_DECODER_FFMPEG, CAMEL_DECODER_SYMPHONIA, CAMEL_DECODER_DR_FLAC,
                                                  CAMEL_DECODER_SNDFILE};
    for (const auto decoderKind : decoders) {
        auto* engine = createEngine();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(loadSource(engine, testutil::testFile("no-md5"), decoderKind), CAMEL_ENGINE_OK);
        auto integrity = flacIntegrity(engine);
        EXPECT_EQ(integrity.status, CAMEL_FLAC_INTEGRITY_UNAVAILABLE) << decoderKind;
        EXPECT_EQ(integrity.has_reference_md5, 0u) << decoderKind;
        destroyEngine(engine);

        engine = createEngine();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(loadSource(engine, testutil::testFile("24-96k"), decoderKind, 100, 180, true), CAMEL_ENGINE_OK);
        std::array<int32_t, 512> samples{};
        uint64_t framesRead = 0;
        EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), 512, &framesRead), CAMEL_ENGINE_OK);
        EXPECT_EQ(framesRead, 80u);
        EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), 512, &framesRead), CAMEL_ENGINE_END);
        EXPECT_EQ(flacIntegrity(engine).status, CAMEL_FLAC_INTEGRITY_INCOMPLETE) << decoderKind;
        destroyEngine(engine);

        for (const auto* name : {"damaged", "truncated"}) {
            engine = createEngine();
            ASSERT_NE(engine, nullptr);
            ASSERT_EQ(loadSource(engine, testutil::testFile(name), decoderKind), CAMEL_ENGINE_OK) << decoderKind << " " << name;
            std::array<int32_t, 4096 * 2> chunk{};
            int32_t status = CAMEL_ENGINE_OK;
            while (status == CAMEL_ENGINE_OK) {
                status = camel_engine_read_decoded_v1(engine, chunk.data(), 4096, &framesRead);
            }
            EXPECT_EQ(status, CAMEL_ENGINE_E_DECODE) << decoderKind << " " << name;
            integrity = flacIntegrity(engine);
            EXPECT_EQ(integrity.status, CAMEL_FLAC_INTEGRITY_FAILED) << decoderKind << " " << name;
            EXPECT_EQ(integrity.has_reference_md5, 1u) << decoderKind << " " << name;
            destroyEngine(engine);
        }
    }
}

TEST(RustEngineHost, FullTrackRangeFinalizesIntegrityAndMissingMd5StaysUnavailable) {
    auto* engine = createEngine();
    ASSERT_NE(engine, nullptr);
    const auto full = camel::audio::testaudio::referenceDecode(testutil::testFile("24-96k"));
    ASSERT_TRUE(full.error.empty()) << full.error;
    ASSERT_EQ(loadSource(engine, testutil::testFile("24-96k"), CAMEL_DECODER_SNDFILE, 0, full.info.totalFrames, true),
              CAMEL_ENGINE_OK);
    std::array<int32_t, 4096 * 2> chunk{};
    uint64_t framesRead = 0;
    int32_t status = CAMEL_ENGINE_OK;
    while (status == CAMEL_ENGINE_OK) status = camel_engine_read_decoded_v1(engine, chunk.data(), 4096, &framesRead);
    ASSERT_EQ(status, CAMEL_ENGINE_END);
    EXPECT_EQ(flacIntegrity(engine).status, CAMEL_FLAC_INTEGRITY_VERIFIED);
    destroyEngine(engine);

    engine = createEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(loadSource(engine, testutil::testFile("no-md5"), CAMEL_DECODER_SNDFILE), CAMEL_ENGINE_OK);
    status = CAMEL_ENGINE_OK;
    while (status == CAMEL_ENGINE_OK) status = camel_engine_read_decoded_v1(engine, chunk.data(), 4096, &framesRead);
    ASSERT_EQ(status, CAMEL_ENGINE_END);
    const auto unavailable = flacIntegrity(engine);
    EXPECT_EQ(unavailable.status, CAMEL_FLAC_INTEGRITY_UNAVAILABLE);
    EXPECT_EQ(unavailable.has_reference_md5, 0u);
    destroyEngine(engine);
}

TEST(RustEngineHost, CorruptAndTruncatedFlacFailIntegrityForEveryDecoder) {
    constexpr std::array<uint32_t, 4> decoders = {CAMEL_DECODER_FFMPEG, CAMEL_DECODER_SYMPHONIA, CAMEL_DECODER_DR_FLAC,
                                                  CAMEL_DECODER_SNDFILE};
    for (const auto* name : {"damaged", "truncated"}) {
        for (const auto decoderKind : decoders) {
            auto* engine = createEngine();
            ASSERT_NE(engine, nullptr);
            ASSERT_EQ(loadSource(engine, testutil::testFile(name), decoderKind), CAMEL_ENGINE_OK) << name << " " << decoderKind;
            std::array<int32_t, 4096 * 2> chunk{};
            uint64_t framesRead = 0;
            int32_t status = CAMEL_ENGINE_OK;
            while (status == CAMEL_ENGINE_OK) status = camel_engine_read_decoded_v1(engine, chunk.data(), 4096, &framesRead);
            EXPECT_EQ(status, CAMEL_ENGINE_E_DECODE) << name << " " << decoderKind;
            const auto integrity = flacIntegrity(engine);
            EXPECT_EQ(integrity.status, CAMEL_FLAC_INTEGRITY_FAILED) << name << " " << decoderKind;
            EXPECT_EQ(integrity.has_reference_md5, 1u) << name << " " << decoderKind;
            destroyEngine(engine);
        }
    }
}

TEST(RustEngineHost, SourceRangeIsEnforced) {
    auto* engine = createEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(loadSource(engine, testutil::testFile("24-96k"), CAMEL_DECODER_SNDFILE, 100, 180, true), CAMEL_ENGINE_OK);
    std::array<int32_t, 512> samples{};
    uint64_t framesRead = 0;
    EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), CAMEL_ENGINE_MAX_RENDER_UNITS + 1, &framesRead),
              CAMEL_ENGINE_E_LIMIT_EXCEEDED);
    EXPECT_EQ(camel_engine_read_decoded_v1(engine, nullptr, 0, &framesRead), CAMEL_ENGINE_OK);
    EXPECT_EQ(framesRead, 0u);
    EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), 512, &framesRead), CAMEL_ENGINE_OK);
    EXPECT_EQ(framesRead, 80u);
    EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), 512, &framesRead), CAMEL_ENGINE_END);
    EXPECT_EQ(framesRead, 0u);
    CamelCommandStampV1 command{sizeof(CamelCommandStampV1), CAMEL_ENGINE_ABI_VERSION, 2, 1, 1};
    EXPECT_EQ(camel_engine_seek_v1(engine, &command, 99), CAMEL_ENGINE_E_SEEK);
    destroyEngine(engine);
}

TEST(RustEngineHost, LoadsUnicodeSourcePathThroughHostAbi) {
    const auto path = testutil::tempDir() / L"\u97F3\u6A02-engine-source.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), path, std::filesystem::copy_options::overwrite_existing);
    auto* engine = createEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(loadSource(engine, path, CAMEL_DECODER_SYMPHONIA), CAMEL_ENGINE_OK);
    std::array<int32_t, 2> samples{};
    uint64_t framesRead = 0;
    EXPECT_EQ(camel_engine_read_decoded_v1(engine, samples.data(), 1, &framesRead), CAMEL_ENGINE_OK);
    EXPECT_EQ(framesRead, 1u);
    const auto reference = camel::audio::testaudio::referenceDecode(path);
    ASSERT_FALSE(reference.samples.empty());
    EXPECT_EQ(samples[0], reference.samples[0]);
    EXPECT_EQ(samples[1], reference.samples[1]);
    destroyEngine(engine);
    std::filesystem::remove(path);
}

} // namespace
