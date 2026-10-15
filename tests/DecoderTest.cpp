// Every decoder against the libFLAC reference (docs/計劃書.md §16.3, §16.7).
#include <gtest/gtest.h>

#include "audio/decode/IDecoder.h"
#include "audio/verify/FlacMd5.h"
#include "TestUtil.h"

using namespace camel::audio;

namespace {

std::vector<int32_t> decodeAll(IDecoder& d) {
    std::vector<int32_t> out;
    std::vector<int32_t> buf(4096 * size_t(d.info().channels));
    while (size_t n = d.read(buf.data(), 4096)) out.insert(out.end(), buf.begin(), buf.begin() + n * d.info().channels);
    return out;
}

class EveryDecoder : public ::testing::TestWithParam<DecoderKind> {};

}  // namespace

TEST_P(EveryDecoder, MatchesReferenceBitForBit) {
    for (const char* name : {"16-44.1k", "24-96k", "20-48k", "no-md5"}) {
        auto file = testutil::testFile(name);
        auto ref = testaudio::referenceDecode(file);
        ASSERT_TRUE(ref.error.empty()) << name << ": " << ref.error;
        if (testutil::knownUnsupported(GetParam(), ref.info.bitsPerSample)) {
            EXPECT_THROW(decodeAll(*openDecoder(GetParam(), file)), Error) << name;
            continue;
        }
        auto dec = openDecoder(GetParam(), file);
        EXPECT_EQ(dec->info().sampleRate, ref.info.sampleRate) << name;
        EXPECT_EQ(dec->info().bitsPerSample, ref.info.bitsPerSample) << name;
        EXPECT_EQ(dec->info().md5.has_value(), ref.info.md5.has_value()) << name;
        auto got = decodeAll(*dec);
        EXPECT_TRUE(got == ref.samples) << name << " differs from the libFLAC reference";
    }
}

TEST_P(EveryDecoder, SeeksSampleExact) {
    auto file = testutil::testFile("24-96k");
    auto ref = testaudio::referenceDecode(file);
    auto dec = openDecoder(GetParam(), file);
    const size_t ch = 2;
    std::vector<int32_t> buf(1000 * ch);
    for (uint64_t target : {0ull, 1ull, 4095ull, 4096ull, 77777ull, 150000ull, 191000ull, 12345ull, 190999ull, 188000ull}) {
        dec->seek(target);
        size_t got = 0;
        while (got < 1000) {
            size_t n = dec->read(buf.data() + got * ch, 1000 - got);
            if (!n) break;
            got += n;
        }
        ASSERT_EQ(got, 1000u) << "at " << target;
        EXPECT_TRUE(std::equal(buf.begin(), buf.end(), ref.samples.begin() + target * ch)) << "seek to " << target << " is not sample exact";
    }
}

TEST_P(EveryDecoder, ThirtyTwoBitEitherDecodesOrFailsCleanly) {
    auto file = testutil::testFile("32-384k");
    auto ref = testaudio::referenceDecode(file);
    try {
        auto dec = openDecoder(GetParam(), file);
        auto got = decodeAll(*dec);
        // A decoder that claims success must be exact.
        EXPECT_TRUE(got == ref.samples);
    } catch (const Error& e) {
        SUCCEED() << decoderName(GetParam()) << " does not support 32-bit FLAC: " << e.what();
    }
}

TEST_P(EveryDecoder, DamagedFilesDoNotCrash) {
    for (const char* name : {"damaged", "truncated"}) {
        try {
            auto dec = openDecoder(GetParam(), testutil::testFile(name));
            decodeAll(*dec);
        } catch (const Error&) {
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Flac, EveryDecoder, ::testing::ValuesIn(kAllDecoders),
                         [](const ::testing::TestParamInfo<DecoderKind>& i) { return std::string(decoderId(i.param)); });

TEST(Decoders, FfmpegHandles32Bit) {
    auto file = testutil::testFile("32-384k");
    auto ref = testaudio::referenceDecode(file);
    ASSERT_TRUE(ref.md5Ok);
    auto dec = openDecoder(DecoderKind::FFmpeg, file);
    EXPECT_TRUE(decodeAll(*dec) == ref.samples);
}
