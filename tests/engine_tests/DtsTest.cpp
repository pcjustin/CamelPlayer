// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: a DTS bitstream posing as 16-bit PCM in a WAVE file. Every WAVE decoder reads it and the DTS stage decodes
// it, but only with "convert when unsupported"; played as PCM it would be loud noise.
#include <gtest/gtest.h>

#include <windows.h>
// windows.h comes first
#include <psapi.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "DtsFixture.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "Engine/decode/FormatProbe.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

const testutil::DtsCd& dtsCd() {
    static const testutil::DtsCd file = testutil::copyDtsCd(testutil::tempDir() / "dts-cd.wav");
    return file;
}

std::filesystem::path dtsPath() {
    dtsCd();
    return testutil::tempDir() / "dts-cd.wav";
}

constexpr float kDecoderTolerance = 1e-5f; // -100 dBFS, between dts_core and FFmpeg's decode

float worstDifference(const float* a, const float* b, size_t count) {
    float worst = 0.0f;
    for (size_t i = 0; i < count; i++) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

std::vector<float> readAll(IDecoder& d) {
    std::vector<float> out, block(size_t{4096} * d.info().channels);
    while (const size_t got = d.readFloat(block.data(), 4096))
        out.insert(out.end(), block.begin(), block.begin() + ptrdiff_t(got * d.info().channels));
    return out;
}

// A 16-bit stereo PCM WAVE file whose data holds a 14-bit DTS sync word at `offsets` (bytes) in silence.
std::filesystem::path syncsInPcm(const char* name, std::initializer_list<size_t> offsets) {
    std::vector<int32_t> words(16384, 0);
    for (const size_t at : offsets) {
        words[at / 2] = 0x1FFF;
        words[at / 2 + 1] = std::bit_cast<int16_t>(uint16_t{0xE800});
        words[at / 2 + 2] = 0x07F1;
    }
    const auto path = testutil::tempDir() / name;
    testaudio::writeWav(path, words, 44100, 16, 2);
    return path;
}

// The fixture with `leading` word frames of zeros before its bitstream and `silence` after it, and with `resumes` the
// bitstream again after those.
std::filesystem::path withSilence(const char* name, uint64_t leading, uint64_t silence, bool resumes) {
    std::ifstream in(dtsPath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t data = bytes.find("data");
    const auto le32 = [&](size_t at) {
        return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 | uint32_t(uint8_t(bytes[at + 2])) << 16 |
               uint32_t(uint8_t(bytes[at + 3])) << 24;
    };
    const auto put32 = [&](size_t at, uint64_t value) {
        for (size_t i = 0; i < 4; i++) bytes[at + i] = char(uint8_t(value >> (8 * i)));
    };
    if (data == std::string::npos || data + 8 + le32(data + 4) != bytes.size())
        throw std::runtime_error("the fixture's data chunk is not its last");
    const std::string stream = bytes.substr(data + 8);
    bytes.insert(data + 8, size_t(leading * 4), '\0');
    bytes += std::string(size_t(silence * 4), '\0');
    if (resumes) bytes += stream;
    put32(data + 4, bytes.size() - data - 8);
    put32(4, bytes.size() - 8);
    const auto path = testutil::tempDir() / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    return path;
}

// A WAVE file whose data is word frames `from` to `to` of the fixture's bitstream played `repeats` times over, as a
// rip of one continuous DTS-CD is cut into tracks.
std::filesystem::path dtsRun(const char* name, uint64_t repeats, uint64_t from, uint64_t to) {
    std::ifstream in(dtsPath(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t data = bytes.find("data");
    if (data == std::string::npos) throw std::runtime_error("the fixture has no data chunk");
    std::string stream;
    for (uint64_t i = 0; i < repeats; i++) stream += bytes.substr(data + 8);
    std::string out = bytes.substr(0, data + 8) + stream.substr(size_t(from * 4), size_t((to - from) * 4));
    const auto put32 = [&](size_t at, uint64_t value) {
        for (size_t i = 0; i < 4; i++) out[at + i] = char(uint8_t(value >> (8 * i)));
    };
    put32(data + 4, out.size() - data - 8);
    put32(4, out.size() - 8);
    const auto path = testutil::tempDir() / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << out;
    return path;
}

// The largest difference between 24-bit stereo `samples` and the downmix of the 5.1 decode `decoded` the Engine plays.
int32_t worstDownmixDifference(const std::vector<int32_t>& samples, const std::vector<float>& decoded) {
    const double a = std::numbers::sqrt2 / 2;
    int32_t worst = 0;
    for (size_t i = 0; i < samples.size() / 2; i++) {
        const float* f = &decoded[i * 6]; // L R C LFE Ls Rs; the LFE is dropped
        const double left = (f[0] + a * f[2] + a * f[4]) * 8388608.0, right = (f[1] + a * f[2] + a * f[5]) * 8388608.0;
        worst = std::max({worst, std::abs(samples[i * 2] - int32_t(std::lround(left))),
                          std::abs(samples[i * 2 + 1] - int32_t(std::lround(right)))});
    }
    return worst;
}

int64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return int64_t(memory.PrivateUsage);
}

} // namespace

// Silence the data chunk holds where the bitstream stops, at its end or before it goes on, is made as it is read: its
// length is the file's to say, not a measure of what one allocation may take.
TEST(Dts, SilenceWhereTheBitstreamStopsTakesBoundedMemory) {
    const uint64_t silence = uint64_t{44100} * 300;
    for (const bool resumes : {false, true}) {
        SCOPED_TRACE(resumes ? "a gap" : "the end");
        const auto path = withSilence(resumes ? "dts-gap.wav" : "dts-tail.wav", 0, silence, resumes);
        const auto d = openDecoder(DecoderKind::Sndfile, path);
        const uint64_t expected = dtsCd().frames * (resumes ? 2 : 1) + silence;
        ASSERT_EQ(d->info().totalFrames, expected);
        std::vector<float> block(size_t{4096} * 6);
        const int64_t before = privateBytes();
        int64_t most = 0;
        uint64_t frames = 0;
        const uint64_t silentFrom = dtsCd().frames, silentTo = silentFrom + silence;
        float inSilence = 0.0f, after = 0.0f;
        while (const size_t got = d->readFloat(block.data(), 4096)) {
            for (size_t i = 0; i < got; i++) {
                const uint64_t at = frames + i;
                // Well inside the silence, past the last frame's tail, and well inside the resumed bitstream.
                for (size_t c = 0; c < 6; c++)
                    if (at >= silentFrom + 4096 && at < silentTo)
                        inSilence = std::max(inSilence, std::abs(block[i * 6 + c]));
                    else if (at >= silentTo + 8192)
                        after = std::max(after, std::abs(block[i * 6 + c]));
            }
            frames += got;
            most = std::max(most, privateBytes() - before);
        }
        EXPECT_EQ(frames, expected);
        EXPECT_EQ(inSilence, 0.0f);
        if (resumes) EXPECT_GT(after, 0.01f) << "the bitstream plays again after the gap";
        EXPECT_LT(most, int64_t{32} << 20) << "bytes held at most";
    }
}

// A rip can start with digital silence before its first DTS frame, longer than the start that is looked at for it.
TEST(Dts, ARipThatStartsWithSilenceIsStillDts) {
    const auto& file = dtsCd();
    for (const uint64_t leading : {uint64_t{44100}, uint64_t{44100} * 30}) {
        SCOPED_TRACE(leading);
        const auto path = withSilence("dts-leading.wav", leading, 0, false);
        ASSERT_EQ(probeFormat(path), AudioFormat::Dts);
        const auto d = openDecoder(DecoderKind::Sndfile, path);
        ASSERT_EQ(d->info().totalFrames, leading + file.frames);
        const auto decoded = readAll(*d);
        ASSERT_EQ(decoded.size(), (leading + file.frames) * 6);
        EXPECT_EQ(*std::max_element(decoded.begin(), decoded.begin() + ptrdiff_t(leading * 6)), 0.0f);
        EXPECT_LT(worstDifference(decoded.data() + leading * 6, file.reference.data(), file.reference.size()), kDecoderTolerance);
    }
    // PCM after as much silence is still PCM.
    std::vector<int32_t> words(size_t{44100} * 2 * 2, 0);
    for (size_t i = size_t{44100} * 2; i < words.size(); i++)
        words[i] = int32_t(std::lround(8000.0 * std::sin(double(i) * 0.01)));
    const auto pcm = testutil::tempDir() / "dts-leading-pcm.wav";
    testaudio::writeWav(pcm, words, 44100, 16, 2);
    EXPECT_EQ(probeFormat(pcm), AudioFormat::Wav);
}

// A frame that does not decode right after the first, as a damaged rip may hold, does not decide the track: the stream
// plays, with silence for that frame, as it would for one further on.
TEST(Dts, AnUndecodableSecondFrameDoesNotRefuseTheTrack) {
    const auto& file = dtsCd();
    std::ifstream in(dtsPath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const size_t data = bytes.find("data");
    ASSERT_NE(data, std::string::npos);
    const std::string sync("\xFF\x1F\x00\xE8", 4);
    const size_t first = bytes.find(sync, data + 8);
    ASSERT_NE(first, std::string::npos);
    const size_t second = bytes.find(sync, first + 2);
    ASSERT_NE(second, std::string::npos);
    ASSERT_EQ(bytes.compare(2 * second - first, sync.size(), sync), 0) << "the frames follow at a steady pace";
    // The second frame keeps its sync word but loses the header after it.
    std::fill_n(bytes.begin() + ptrdiff_t(second + 6), 12, '\0');
    const auto path = testutil::tempDir() / "dts-second-damaged.wav";
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    ASSERT_EQ(probeFormat(path), AudioFormat::Dts);
    std::unique_ptr<IDecoder> d;
    ASSERT_NO_THROW(d = openDecoder(DecoderKind::Sndfile, path));
    EXPECT_EQ(d->info().channels, 6u);
    EXPECT_EQ(readAll(*d).size(), file.frames * 6);
}

// A WAVE file that looks like DTS at its start, but whose frames all fail to decode, is refused after a bounded read: a
// rip of an hour holds hundreds of megabytes, which may be on a NAS.
TEST(Dts, AWaveWithNoFrameThatDecodesIsRefusedWithoutReadingItWhole) {
    std::ifstream in(dtsPath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const size_t data = bytes.find("data");
    ASSERT_NE(data, std::string::npos);
    // Every frame keeps its sync word but loses the header after it.
    const std::string sync("\xFF\x1F\x00\xE8", 4);
    for (size_t at = bytes.find(sync, data + 8); at != std::string::npos; at = bytes.find(sync, at + 2))
        std::fill_n(bytes.begin() + ptrdiff_t(at + 6), 12, '\0');
    // Then 100 MB of noise, which holds no sync word at a word's start.
    constexpr size_t kNoise = size_t{100} << 20;
    const uint64_t total = bytes.size() - data - 8 + kNoise;
    const auto put32 = [&](size_t at, uint64_t value) {
        for (size_t i = 0; i < 4; i++) bytes[at + i] = char(uint8_t(value >> (8 * i)));
    };
    put32(data + 4, total);
    put32(4, data + total);
    const auto path = testutil::tempDir() / "dts-undecodable.wav";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
        std::string noise(size_t{1} << 20, '\0');
        for (size_t i = 0; i < noise.size(); i += 2) noise[i] = char(0x40 + i % 61);
        for (size_t written = 0; written < kNoise; written += noise.size()) out << noise;
    }
    ASSERT_EQ(probeFormat(path), AudioFormat::Dts);
    IO_COUNTERS before{}, after{};
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
    EXPECT_THROW(openDecoder(DecoderKind::Sndfile, path), Error);
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
    EXPECT_LT(after.ReadTransferCount - before.ReadTransferCount, uint64_t{32} << 20) << "bytes read";
    std::filesystem::remove(path);
}

TEST(Dts, ProbeFindsDtsInsideWave) {
    EXPECT_EQ(probeFormat(dtsPath()), AudioFormat::Dts);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-steady.wav", {12, 4108, 8204})), AudioFormat::Dts);
    // PCM that happens to hold the pattern, but not as a steady stream of frames.
    EXPECT_EQ(probeFormat(syncsInPcm("dts-one.wav", {0})), AudioFormat::Wav);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-two.wav", {0, 4096})), AudioFormat::Wav);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-uneven.wav", {0, 4096, 9000})), AudioFormat::Wav);
}

TEST(Dts, EveryWaveDecoderReadsTheSameDecode) {
    const auto& file = dtsCd();
    ASSERT_GT(file.reference.size(), 6u * 20000);
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::DrWav, DecoderKind::Symphonia}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        EXPECT_TRUE(d->readsFloat());
        EXPECT_EQ(d->info().sampleRate, 44100u);
        EXPECT_EQ(d->info().channels, 6u);
        EXPECT_EQ(d->info().channelMask, 0x3Fu) << "L R C LFE and the surrounds";
        EXPECT_EQ(d->info().bitsPerSample, 24u);
        EXPECT_EQ(d->info().totalFrames, file.frames);
        const auto decoded = readAll(*d);
        ASSERT_EQ(decoded.size(), file.frames * 6);
        EXPECT_LT(worstDifference(decoded.data(), file.reference.data(), decoded.size()), kDecoderTolerance);
    }
}

TEST(Dts, SeeksToTheExactSample) {
    const auto& file = dtsCd();
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::Symphonia}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        for (const uint64_t target : {uint64_t(0), uint64_t(777), file.frames / 2 + 123, file.frames - 300}) {
            d->seek(target);
            std::vector<float> block(size_t{256} * 6);
            ASSERT_EQ(d->readFloat(block.data(), 256), 256u) << target;
            EXPECT_LT(worstDifference(block.data(), file.reference.data() + target * 6, block.size()), kDecoderTolerance)
                << target;
        }
    }
}

namespace {

struct Played {
    EngineSnapshot snapshot;
    std::optional<OutputPlan> plan;
    std::vector<int32_t> samples;
};

Played playOnInt24(bool convert) {
    const auto out = testutil::tempDir() / "dts-capture.raw";
    std::filesystem::remove(out);
    Settings settings;
    settings.convertWhenUnsupported = convert;
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.sampleRate == 44100 && f.type == SampleType::Int24; };
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(dtsPath());
        engine.waitIdle();
        played.plan = started.get().value_or(engine.snapshot()).plan;
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        played.snapshot = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    played.samples.resize(bytes.size() / 3);
    unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), played.samples.size(), 24, SampleType::Int24,
                  played.samples.data());
    return played;
}

} // namespace

TEST(Dts, PlaysOnlyWithConversionAndThenDownmixes) {
    const auto& file = dtsCd();
    const auto refused = playOnInt24(false);
    EXPECT_EQ(refused.snapshot.error, ErrorCode::FormatNotSupported);
    EXPECT_NE(refused.snapshot.errorDetail.find("convert when unsupported"), std::string::npos) << refused.snapshot.errorDetail;
    EXPECT_TRUE(refused.samples.empty());

    const auto played = playOnInt24(true);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_TRUE(played.plan->downmix);
    EXPECT_EQ(played.plan->state, PipelineState::Processed);
    EXPECT_EQ(played.snapshot.format, AudioFormat::Dts);
    const auto& samples = played.samples;
    ASSERT_EQ(samples.size(), file.frames * 2);
    EXPECT_LE(worstDownmixDifference(samples, readAll(*openDecoder(DecoderKind::Sndfile, dtsPath()))),
              2); // TPDF dither of one LSB and float rounding
}

// A continuous DTS-CD ripped track by track cuts a frame at each track change. Told which track follows, the first
// decodes the frame's beginning with the next file's bytes, and the next decodes its rest after the first's last frames,
// with the filters as they were: both play as the whole file does.
TEST(Dts, AFrameCutBetweenTwoTracksPlaysInBoth) {
    const uint64_t words = dtsCd().frames, cut = 512 * 20 + 200; // 200 word frames into a frame
    const auto whole = dtsRun("dts-whole.wav", 1, 0, words);
    const auto first = dtsRun("dts-first.wav", 1, 0, cut), second = dtsRun("dts-second.wav", 1, cut, words);
    const auto expected = readAll(*openDecoder(DecoderKind::Sndfile, whole));
    ASSERT_EQ(expected.size(), words * 6);
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::Symphonia}) {
        SCOPED_TRACE(decoderName(kind));
        const auto a = openDecoder(kind, first), b = openDecoder(kind, second);
        a->playsOnInto(second);
        b->followsOn(first);
        const auto gotA = readAll(*a), gotB = readAll(*b);
        ASSERT_EQ(gotA.size(), cut * 6);
        ASSERT_EQ(gotB.size(), (words - cut) * 6);
        EXPECT_EQ(worstDifference(gotA.data(), expected.data(), gotA.size()), 0.0f);
        EXPECT_EQ(worstDifference(gotB.data(), expected.data() + cut * 6, gotB.size()), 0.0f);
        b->seek(100);
        std::vector<float> block(size_t{256} * 6);
        ASSERT_EQ(b->readFloat(block.data(), 256), 256u);
        EXPECT_EQ(worstDifference(block.data(), expected.data() + (cut + 100) * 6, block.size()), 0.0f) << "after a seek";
    }
}

// A track that does not follow on is not joined, though it is DTS and starts within a frame.
TEST(Dts, AnotherTrackIsNotJoined) {
    const uint64_t words = dtsCd().frames, cut = 512 * 20 + 200;
    const auto first = dtsRun("dts-alone-first.wav", 1, 0, cut), other = dtsRun("dts-other.wav", 1, 333, words);
    const auto alone = readAll(*openDecoder(DecoderKind::Sndfile, first));
    const auto otherAlone = readAll(*openDecoder(DecoderKind::Sndfile, other));
    const auto a = openDecoder(DecoderKind::Sndfile, first), b = openDecoder(DecoderKind::Sndfile, other);
    a->playsOnInto(other);
    b->followsOn(first);
    EXPECT_EQ(readAll(*a), alone);
    EXPECT_EQ(readAll(*b), otherAlone);
}

// The Engine tells a track queued to play on of the track before it.
TEST(Dts, TwoTracksOfOneRipPlayOnAsTheWholeFile) {
    const uint64_t words = dtsCd().frames, repeats = 8, cut = 6 * words + uint64_t{512} * 7 + 300;
    const auto whole = dtsRun("dts-rip.wav", repeats, 0, repeats * words);
    const auto first = dtsRun("dts-rip-1.wav", repeats, 0, cut), second = dtsRun("dts-rip-2.wav", repeats, cut, repeats * words);
    const auto out = testutil::tempDir() / "dts-rip.raw";
    std::filesystem::remove(out);
    Settings settings;
    settings.convertWhenUnsupported = true;
    settings.wasapi.timing.predecodeSeconds = 1; // the next track is queued before the first is read to its end
    CaptureOptions options;
    options.file = out;
    options.realtime = true;
    options.accepts = [](const DeviceFormat& f) { return f.sampleRate == 44100 && f.type == SampleType::Int24; };
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(first);
        engine.waitIdle();
        bool queued = false;
        for (int i = 0; i < 400 && !queued; ++i) {
            queued = engine.queueNext(second);
            if (!queued) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(queued);
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        EXPECT_EQ(engine.snapshot().transitions, 1u);
        EXPECT_FALSE(engine.snapshot().error) << engine.snapshot().errorDetail;
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::vector<int32_t> samples(bytes.size() / 3);
    unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), samples.size(), 24, SampleType::Int24, samples.data());
    ASSERT_EQ(samples.size(), repeats * words * 2);
    EXPECT_LE(worstDownmixDifference(samples, readAll(*openDecoder(DecoderKind::Sndfile, whole))), 2);
    std::filesystem::remove(out);
}
