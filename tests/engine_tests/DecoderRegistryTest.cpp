// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The decoder registry (docs/Extension-points.md #1, #2): built-in decoders keep their settings IDs, order and defaults;
// a decoder registered at runtime opens through the same path and is refused for formats it does not declare.
#include <gtest/gtest.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/DecoderRegistry.h"
#include "DsdFixture.h"
#include "Engine/decode/DecoderHost.h"
#include "Engine/decode/IDecoder.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

std::vector<std::string> ids(std::string_view format) {
    std::vector<std::string> out;
    for (const auto& d : decoderRegistry().forFormat(format)) out.push_back(d.id);
    return out;
}

// A second name for libsndfile, as an extension decoder would appear.
const char* registerAlias() {
    static std::once_flag once;
    std::call_once(once, [] {
        decoderRegistry().add({"test/sndfile-alias",
                               "Test alias",
                               {"flac"},
                               std::nullopt,
                               [](const std::filesystem::path& path, std::string_view, uint32_t) {
                                   return openDecoder(DecoderKind::Sndfile, path);
                               }});
    });
    return "test/sndfile-alias";
}

std::vector<int32_t> decodeAll(IDecoder& d) {
    std::vector<int32_t> out;
    std::vector<int32_t> chunk(4096 * d.info().channels);
    while (const size_t frames = d.read(chunk.data(), 4096))
        out.insert(out.end(), chunk.begin(), chunk.begin() + frames * d.info().channels);
    return out;
}

} // namespace

TEST(DecoderRegistry, BuiltInIdsOrderAndDefaultsAreUnchanged) {
    EXPECT_EQ(ids("flac"), (std::vector<std::string>{"ffmpeg", "symphonia", "drflac", "sndfile"}));
    EXPECT_EQ(ids("alac"), (std::vector<std::string>{"ffmpeg", "symphonia"}));
    for (const char* format : {"wav", "aiff"})
        EXPECT_EQ(ids(format), (std::vector<std::string>{"ffmpeg", "symphonia", "sndfile", "drwav"})) << format;
    EXPECT_EQ(ids("aac"), (std::vector<std::string>{"ffmpeg", "symphonia"}));
    EXPECT_EQ(ids("mp3"), (std::vector<std::string>{"ffmpeg", "symphonia", "mpg123", "drmp3"}));
    EXPECT_EQ(ids("dsd"), (std::vector<std::string>{"dsdnexus"}));
    EXPECT_TRUE(ids("opus").empty());
    EXPECT_EQ(defaultDecoder(AudioFormat::Flac), DecoderKind::Sndfile);
    EXPECT_EQ(defaultDecoder(AudioFormat::Alac), DecoderKind::Symphonia);
    EXPECT_EQ(defaultDecoder(AudioFormat::Aac), DecoderKind::Symphonia);
    EXPECT_EQ(defaultDecoder(AudioFormat::Mp3), DecoderKind::Mpg123);
    EXPECT_EQ(defaultDecoder(AudioFormat::Dsd), DecoderKind::DsdNexus);
    for (const DecoderKind kind : kAllDecoders) EXPECT_EQ(decoderRegistry().find(decoderId(kind))->builtin, kind);
}

TEST(DecoderRegistry, RejectsDuplicateAndUnknownIds) {
    EXPECT_THROW(decoderRegistry().add({"sndfile", "again", {"flac"}, std::nullopt, {}}), std::invalid_argument);
    EXPECT_THROW(decoderRegistry().setOpener("no-such-decoder", {}), std::invalid_argument);
    EXPECT_FALSE(decoderRegistry().find("no-such-decoder"));
    EXPECT_THROW(openDecoder(std::string_view("no-such-decoder"), testutil::testFile("16-44.1k")), Error);
}

TEST(DecoderRegistry, RegisteredDecoderOpensLikeABuiltInOnlyForItsFormats) {
    const char* alias = registerAlias();
    EXPECT_EQ(ids("flac").back(), alias);
    EXPECT_EQ(decodersFor(AudioFormat::Flac).size(), 4u); // the built-in choice list is unchanged
    const auto file = testutil::testFile("16-44.1k");
    auto viaAlias = openDecoder(std::string_view(alias), file);
    auto direct = openDecoder(DecoderKind::Sndfile, file);
    EXPECT_EQ(viaAlias->info().totalFrames, direct->info().totalFrames);
    EXPECT_EQ(decodeAll(*viaAlias), decodeAll(*direct));

    const auto dsf = testutil::tempDir() / "decoder-registry.dsf";
    dsdfixture::writeFile(dsf, dsdfixture::dsf(dsdfixture::rawDsd(1000, 2, 5), {}));
    try {
        openDecoder(std::string_view(alias), dsf);
        FAIL() << "a decoder must not open a format it does not declare";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::UnsupportedStream);
        EXPECT_EQ(std::string(e.what()), "Test alias cannot decode DSD");
    }
}

TEST(DecoderRegistry, AbiNumbersKeepBuiltInsAndNumberRegisteredDecoders) {
    EXPECT_EQ(abiDecoder("ffmpeg"), CAMEL_DECODER_FFMPEG);
    EXPECT_EQ(abiDecoder("sndfile"), CAMEL_DECODER_SNDFILE);
    EXPECT_EQ(abiDecoder("dsdnexus"), CAMEL_DECODER_DSD_NEXUS);
    EXPECT_EQ(abiDecoder("no-such-decoder"), 0u);
    const char* alias = registerAlias();
    EXPECT_EQ(abiDecoder(alias), CAMEL_DECODER_REGISTERED_BASE + *decoderRegistry().indexOf(alias));
}
