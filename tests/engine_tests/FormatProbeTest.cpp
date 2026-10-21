// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Container and codec detection from file contents (docs/Plan.md §3.1).
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "Engine/decode/FormatProbe.h"
#include "TestUtil.h"

using namespace camel::audio;

namespace {

using Bytes = std::string;

Bytes be32(uint32_t v) {
    return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}

Bytes box(const char* type, const Bytes& payload) {
    return be32(uint32_t(8 + payload.size())) + type + payload;
}

Bytes descriptor(uint8_t tag, const Bytes& payload) {
    return Bytes(1, char(tag)) + char(payload.size()) + payload;
}

Bytes esds(uint8_t objectType, const Bytes& audioSpecificConfig) {
    const Bytes config = descriptor(0x04, Bytes(1, char(objectType)) + Bytes(12, '\0') + descriptor(0x05, audioSpecificConfig));
    return box("esds", be32(0) + descriptor(0x03, Bytes("\0\1\0", 3) + config));
}

// A QuickTime sound sample entry of `version` with the given child boxes.
Bytes soundEntry(const char* codec, const Bytes& children, uint16_t version = 0) {
    Bytes fields(28, '\0'); // reserved, data reference index, version ... sample rate
    fields[8] = char(version >> 8);
    fields[9] = char(version);
    if (version == 1) fields += Bytes(16, '\0');
    if (version == 2) fields += Bytes(36, '\0');
    return box(codec, fields + children);
}

Bytes mp4(const Bytes& entry, const char* handler = "soun", bool mdatFirst = false) {
    const Bytes hdlr = box("hdlr", be32(0) + be32(0) + handler + Bytes(12, '\0'));
    const Bytes stsd = box("stsd", be32(0) + be32(1) + entry);
    const Bytes trak = box("trak", box("tkhd", Bytes(84, '\0')) +
                                       box("mdia", box("mdhd", Bytes(24, '\0')) + hdlr + box("minf", box("stbl", stsd))));
    const Bytes ftyp = box("ftyp", Bytes("M4A ") + be32(0));
    const Bytes moov = box("moov", box("mvhd", Bytes(100, '\0')) + trak);
    const Bytes mdat = box("mdat", Bytes(64, '\x55'));
    return mdatFirst ? ftyp + mdat + moov : ftyp + moov + mdat;
}

Bytes id3(size_t size) {
    return Bytes("ID3\4\0\0", 6) + char((size >> 21) & 0x7f) + char((size >> 14) & 0x7f) + char((size >> 7) & 0x7f) +
           char(size & 0x7f) + Bytes(size, '\0');
}

std::filesystem::path write(const std::string& name, const Bytes& bytes) {
    const auto path = testutil::tempDir() / ("probe-" + name);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    return path;
}

AudioFormat probe(const std::string& name, const Bytes& bytes) {
    return probeFormat(write(name, bytes));
}

void expectUnsupported(const std::string& name, const Bytes& bytes) {
    try {
        probe(name, bytes);
        ADD_FAILURE() << name << " was accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::UnsupportedStream) << name << ": " << e.what();
    }
}

const Bytes kAacLc("\x12\x10", 2); // audio object type 2, 44.1 kHz, stereo
const Bytes kHeAac("\x2B\x92", 2); // audio object type 5

} // namespace

TEST(FormatProbe, IdentifiesContainersByContentNotExtension) {
    EXPECT_EQ(probeFormat(testutil::testFile("24-96k")), AudioFormat::Flac);
    std::ifstream flac(testutil::testFile("16-44.1k"), std::ios::binary);
    const Bytes flacBytes((std::istreambuf_iterator<char>(flac)), std::istreambuf_iterator<char>());
    EXPECT_EQ(probe("flac-named.mp3", flacBytes), AudioFormat::Flac);
    EXPECT_EQ(probe("id3.flac", id3(300) + flacBytes), AudioFormat::Flac);
    EXPECT_EQ(probe("riff.flac", "RIFF" + be32(4) + "WAVEfmt "), AudioFormat::Wav);
    EXPECT_EQ(probe("rf64.wav", "RF64" + be32(4) + "WAVEds64"), AudioFormat::Wav);
    EXPECT_EQ(probe("aiff", "FORM" + be32(4) + "AIFFCOMM"), AudioFormat::Aiff);
    EXPECT_EQ(probe("aifc", "FORM" + be32(4) + "AIFCFVER"), AudioFormat::Aiff);
    EXPECT_EQ(probe("dsf", Bytes("DSD \x1c\0\0\0\0\0\0\0", 12)), AudioFormat::Dsd);
    EXPECT_EQ(probe("dff", "FRM8" + Bytes(8, '\0') + "DSD "), AudioFormat::Dsd);
}

TEST(FormatProbe, SeparatesMpegAudioFromAdtsAndChecksTheAacProfile) {
    EXPECT_EQ(probe("mp3", Bytes("\xFF\xFB\x90\x64", 4) + Bytes(16, '\0')), AudioFormat::Mp3);
    EXPECT_EQ(probe("id3.mp3", id3(40) + Bytes("\xFF\xF3\x90\x64", 4) + Bytes(16, '\0')), AudioFormat::Mp3);
    EXPECT_EQ(probe("adts-lc", Bytes("\xFF\xF1\x50\x80", 4) + Bytes(16, '\0')), AudioFormat::Aac);
    expectUnsupported("adts-main", Bytes("\xFF\xF1\x10\x80", 4) + Bytes(16, '\0'));
    expectUnsupported("mp2", Bytes("\xFF\xFD\x90\x64", 4) + Bytes(16, '\0'));
}

TEST(FormatProbe, SeparatesM4aAlacFromAacLc) {
    EXPECT_EQ(probe("alac.m4a", mp4(soundEntry("alac", box("alac", Bytes(28, '\0'))))), AudioFormat::Alac);
    EXPECT_EQ(probe("aac.m4a", mp4(soundEntry("mp4a", esds(0x40, kAacLc)))), AudioFormat::Aac);
    EXPECT_EQ(probe("aac-late-moov.m4a", mp4(soundEntry("mp4a", esds(0x40, kAacLc)), "soun", true)), AudioFormat::Aac);
    EXPECT_EQ(probe("aac-mpeg2.m4a", mp4(soundEntry("mp4a", esds(0x67, kAacLc)))), AudioFormat::Aac);
    EXPECT_EQ(probe("aac-qt1.m4a", mp4(soundEntry("mp4a", box("wave", esds(0x40, kAacLc)), 1))), AudioFormat::Aac);
    EXPECT_EQ(probe("aac-qt2.m4a", mp4(soundEntry("mp4a", esds(0x40, kAacLc), 2))), AudioFormat::Aac);
    EXPECT_EQ(probe("mp3.m4a", mp4(soundEntry("mp4a", esds(0x6B, Bytes(1, '\0'))))), AudioFormat::Mp3);
}

TEST(FormatProbe, RejectsUnsupportedProfilesCodecsAndDamagedContainers) {
    expectUnsupported("he-aac.m4a", mp4(soundEntry("mp4a", esds(0x40, kHeAac))));
    expectUnsupported("vorbis.m4a", mp4(soundEntry("mp4a", esds(0xDD, kAacLc))));
    expectUnsupported("opus.m4a", mp4(soundEntry("Opus", Bytes(8, '\0'))));
    expectUnsupported("video.m4a", mp4(soundEntry("alac", Bytes()), "vide"));
    expectUnsupported("no-esds.m4a", mp4(soundEntry("mp4a", Bytes())));
    Bytes truncated = mp4(soundEntry("alac", Bytes()));
    truncated.resize(truncated.size() - 120);
    expectUnsupported("truncated.m4a", truncated);
    expectUnsupported("ftyp-only.m4a", box("ftyp", Bytes("M4A ") + be32(0)));
    expectUnsupported("riff-avi", "RIFF" + be32(4) + "AVI LIST");
    expectUnsupported("text", "plain text, not audio");
    expectUnsupported("short", "fLa");
    expectUnsupported("id3-only", id3(20));
}

TEST(FormatProbe, MissingFileIsAnOpenError) {
    try {
        probeFormat(testutil::tempDir() / "probe-missing.flac");
        ADD_FAILURE() << "missing file was accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::FileOpen);
    }
}

// Local real-world samples (testdata/, not in Git): each must be classified, never crash.
TEST(FormatProbe, RealSamples) {
    std::filesystem::path testdata;
    for (auto d = std::filesystem::current_path(); !d.empty() && d != d.root_path(); d = d.parent_path())
        if (std::filesystem::exists(d / "testdata" / "sample-6s.wav")) {
            testdata = d / "testdata";
            break;
        }
    if (testdata.empty()) GTEST_SKIP() << "testdata/ not present";
    const std::pair<const char*, AudioFormat> expected[] = {{"Sample_BeeMoved_96kHz24bit.flac", AudioFormat::Flac},
                                                            {"sample-6s.wav", AudioFormat::Wav},
                                                            {"sample-6s.mp3", AudioFormat::Mp3},
                                                            {"sample-2.alac", AudioFormat::Alac}};
    for (const auto& [name, format] : expected)
        if (std::filesystem::exists(testdata / name)) EXPECT_EQ(probeFormat(testdata / name), format) << name;
    for (const auto& entry : std::filesystem::directory_iterator(testdata)) {
        const auto ext = entry.path().extension();
        if (ext == ".dsf") EXPECT_EQ(probeFormat(entry.path()), AudioFormat::Dsd) << entry.path().filename();
        if (ext == ".m4a") {
            const auto format = probeFormat(entry.path());
            EXPECT_TRUE(format == AudioFormat::Aac || format == AudioFormat::Alac) << entry.path().filename();
            RecordProperty(entry.path().filename().string(), formatId(format));
        }
    }
}
