#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <limits>

#include "audio/decode/IDecoder.h"
#include "audio/verify/FlacMd5.h"
#include "TestUtil.h"

using namespace camel::audio;

namespace {

class DecoderContract : public ::testing::TestWithParam<DecoderKind> {};

TEST(DecoderSelection, InvalidKindIsRejectedWithoutDereferencingNull) {
    EXPECT_THROW(openDecoder(static_cast<DecoderKind>(255), testutil::testFile("24-96k")), Error);
}

TEST_P(DecoderContract, CompleteDecodeRejectsWrongMd5AndTruncatedAudio) {
    const auto file = testutil::tempDir() / (std::string("invalid-integrity-") + decoderId(GetParam()) + ".flac");
    std::filesystem::copy_file(testutil::testFile("24-96k"), file, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(26);
        char byte = 0;
        stream.read(&byte, 1);
        byte ^= 0x5A;
        stream.seekp(26);
        stream.write(&byte, 1);
    }
    auto completeDecode = [&](const std::filesystem::path& path) {
        auto decoder = openDecoder(GetParam(), path);
        std::array<int32_t, 8192> samples{};
        while (decoder->read(samples.data(), 4096)) {
        }
    };
    EXPECT_THROW(completeDecode(file), Error);
    EXPECT_THROW(completeDecode(testutil::testFile("truncated")), Error);
    std::filesystem::remove(file);
}

TEST_P(DecoderContract, OutOfRangeSeekReportsSeekFailureAndKeepsDecoderUsable) {
    auto decoder = openDecoder(GetParam(), testutil::testFile("24-96k"));
    try {
        decoder->seek(std::numeric_limits<uint64_t>::max());
        FAIL() << "Out-of-range seek accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::SeekFailed);
    }
    std::array<int32_t, 2> samples{};
    EXPECT_EQ(decoder->read(samples.data(), 1), 1u);
}

TEST_P(DecoderContract, MissingAndNonAudioFilesFailCleanly) {
    const auto missing = testutil::tempDir() / "decoder-contract-missing.flac";
    EXPECT_THROW(openDecoder(GetParam(), missing), Error);
    const auto file = testutil::tempDir() / "decoder-contract-invalid.flac";
    {
        std::ofstream out(file, std::ios::binary);
        out << "This is not an audio stream.";
    }
    EXPECT_THROW(openDecoder(GetParam(), file), Error);
    std::filesystem::remove(file);
}

TEST_P(DecoderContract, SmallReadsRespectBufferAndProduceStreamMd5) {
    for (const auto* name : {"16-44.1k", "24-96k"}) {
        auto decoder = openDecoder(GetParam(), testutil::testFile(name));
        ASSERT_TRUE(decoder->info().md5);
        FlacMd5 md5(decoder->info().bitsPerSample);
        uint64_t frames = 0;
        for (size_t request : {1u, 7u, 4097u, 13u}) {
            std::vector<int32_t> samples(request * 2 + 2, 0x12345678);
            const auto got = decoder->read(samples.data() + 1, request);
            ASSERT_LE(got, request);
            EXPECT_EQ(samples.front(), 0x12345678);
            EXPECT_EQ(samples.back(), 0x12345678);
            md5.update(samples.data() + 1, got * 2);
            frames += got;
        }
        std::array<int32_t, 1026> samples;
        for (;;) {
            samples.fill(0x12345678);
            const auto got = decoder->read(samples.data() + 1, 512);
            ASSERT_LE(got, 512u);
            EXPECT_EQ(samples.front(), 0x12345678);
            EXPECT_EQ(samples.back(), 0x12345678);
            if (!got) break;
            md5.update(samples.data() + 1, got * 2);
            frames += got;
        }
        EXPECT_EQ(frames, decoder->info().totalFrames);
        EXPECT_EQ(md5.finish(), *decoder->info().md5);
        EXPECT_EQ(decoder->read(samples.data(), 1), 0u);
        EXPECT_EQ(decoder->read(samples.data(), 1), 0u);
    }
}

TEST_P(DecoderContract, ZeroLengthReadDoesNotConsumeAudio) {
    auto decoder = openDecoder(GetParam(), testutil::testFile("24-96k"));
    std::array<int32_t, 2> samples{};
    EXPECT_EQ(decoder->read(samples.data(), 0), 0u);
    ASSERT_EQ(decoder->read(samples.data(), 1), 1u);
    const auto reference = testaudio::referenceDecode(testutil::testFile("24-96k"));
    ASSERT_GE(reference.samples.size(), 2u);
    EXPECT_EQ(samples[0], reference.samples[0]);
    EXPECT_EQ(samples[1], reference.samples[1]);
}

TEST_P(DecoderContract, SeekAfterEofRecoversAndLastFrameIsExact) {
    const auto file = testutil::testFile("24-96k");
    auto decoder = openDecoder(GetParam(), file);
    const auto reference = testaudio::referenceDecode(file);
    std::array<int32_t, 8192> samples{};
    while (decoder->read(samples.data(), 4096)) {
    }
    decoder->seek(decoder->info().totalFrames - 1);
    ASSERT_EQ(decoder->read(samples.data(), 1), 1u);
    EXPECT_EQ(samples[0], reference.samples[reference.samples.size() - 2]);
    EXPECT_EQ(samples[1], reference.samples.back());
    EXPECT_EQ(decoder->read(samples.data(), 1), 0u);
    decoder->seek(0);
    ASSERT_EQ(decoder->read(samples.data(), 1), 1u);
    EXPECT_EQ(samples[0], reference.samples[0]);
    EXPECT_EQ(samples[1], reference.samples[1]);
}

TEST_P(DecoderContract, UnicodeFilePathsDecodeIdentically) {
    const auto copy = testutil::tempDir() / L"\u97F3\u6A02-contract.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), copy, std::filesystem::copy_options::overwrite_existing);
    {
        auto decoder = openDecoder(GetParam(), copy);
        std::array<int32_t, 2> samples{};
        EXPECT_EQ(decoder->read(samples.data(), 1), 1u);
        EXPECT_EQ(decoder->info().bitsPerSample, 16u);
    }
    std::filesystem::remove(copy);
}

INSTANTIATE_TEST_SUITE_P(Flac, DecoderContract, ::testing::ValuesIn(kAllDecoders),
                         [](const ::testing::TestParamInfo<DecoderKind>& info) { return std::string(decoderId(info.param)); });

} // namespace
