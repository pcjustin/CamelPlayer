#include "Engine/testing/TestAudio.h"

#define FLAC__NO_DLL
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "Common/util/Text.h"

namespace camel::audio::testaudio {

std::vector<GenSpec> standardMatrix(double seconds) {
    return {
        {"16-44.1k", 44100, 16, 2, seconds}, {"16-48k", 48000, 16, 2, seconds},     {"24-96k", 96000, 24, 2, seconds},
        {"24-192k", 192000, 24, 2, seconds}, {"24-352.8k", 352800, 24, 2, seconds}, {"24-384k", 384000, 24, 2, seconds},
        {"24-768k", 768000, 24, 2, seconds},
    };
}

std::vector<int32_t> makeSignal(const GenSpec& spec) {
    if (!spec.sampleRate || !spec.channels || spec.channels > 8 || spec.bits < 4 || spec.bits > 32 ||
        !std::isfinite(spec.seconds) || spec.seconds < 0 || !std::isfinite(spec.toneDb))
        throw std::invalid_argument("invalid test signal parameters");
    const double sampleCount = spec.seconds * spec.sampleRate * spec.channels;
    if (!std::isfinite(sampleCount) || sampleCount >= double(std::numeric_limits<size_t>::max()) / sizeof(int32_t))
        throw std::length_error("test signal is too large");
    const uint64_t frames = uint64_t(spec.seconds * spec.sampleRate);
    std::vector<int32_t> out(size_t(frames * spec.channels));
    const double full = std::ldexp(1.0, int(spec.bits) - 1) - 1.0;
    uint64_t lcg = 0x9E3779B97F4A7C15ull ^ (uint64_t(spec.sampleRate) << 8) ^ spec.bits;
    for (uint64_t i = 0; i < frames; i++) {
        for (uint32_t c = 0; c < spec.channels; c++) {
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const double noise = double(int64_t(lcg >> 11) - (int64_t(1) << 52)) / double(int64_t(1) << 52);
            const double t = double(i) / spec.sampleRate;
            if (spec.toneDb < 0) {
                const double ramp = std::min(1.0, double(i) / (0.05 * spec.sampleRate));
                const double a = std::pow(10.0, spec.toneDb / 20.0) * ramp;
                out[size_t(i * spec.channels + c)] =
                    int32_t(std::llround(a * full * std::sin(2 * 3.14159265358979 * 440.0 * double(i) / spec.sampleRate)));
                continue;
            }
            double v = 0.6 * std::sin(2 * 3.14159265358979 * (997.0 + 110.0 * c) * t) + 0.3 * noise;
            if (i % 50000 == 7) v = (c ? -1.0 : 1.0); // touch both extremes now and then
            int64_t s = std::llround(v * full);
            const int64_t lo = -(int64_t(1) << (spec.bits - 1)), hi = (int64_t(1) << (spec.bits - 1)) - 1;
            out[size_t(i * spec.channels + c)] = int32_t(std::clamp(s, lo, hi));
        }
    }
    return out;
}

void writeFlacSamples(const std::filesystem::path& path, const std::vector<int32_t>& samples, uint32_t sampleRate, uint32_t bits,
                      uint32_t channels) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) throw std::runtime_error("FLAC__stream_encoder_new failed");
    FLAC__stream_encoder_set_channels(enc, channels);
    FLAC__stream_encoder_set_bits_per_sample(enc, bits);
    FLAC__stream_encoder_set_sample_rate(enc, sampleRate);
    FLAC__stream_encoder_set_streamable_subset(enc, false); // rates above 48 kHz with large blocks are outside the subset
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, samples.size() / channels);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w+b") != 0 || !f) {
        FLAC__stream_encoder_delete(enc);
        throw std::runtime_error("cannot create " + toUtf8(path.wstring()));
    }
    // init_FILE takes ownership of `f`
    if (FLAC__stream_encoder_init_FILE(enc, f, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(enc);
        throw std::runtime_error("FLAC encoder init failed");
    }
    const size_t chunk = 4096;
    const size_t frames = samples.size() / channels;
    bool ok = true;
    for (size_t at = 0; at < frames && ok; at += chunk) {
        size_t n = std::min(chunk, frames - at);
        ok = FLAC__stream_encoder_process_interleaved(enc, samples.data() + at * channels, unsigned(n));
    }
    std::string state = FLAC__stream_encoder_get_resolved_state_string(enc);
    ok = FLAC__stream_encoder_finish(enc) && ok;
    FLAC__stream_encoder_delete(enc);
    if (!ok) throw std::runtime_error("FLAC encoding failed: " + state);
}

void writeFlac(const std::filesystem::path& path, const GenSpec& spec) {
    writeFlacSamples(path, makeSignal(spec), spec.sampleRate, spec.bits, spec.channels);
    if (!spec.md5) { // libFLAC has no public switch for this; zero the STREAMINFO MD5 (bytes 26..41) like encoders that skip it
        std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
        const char zeros[16] = {};
        io.seekp(26);
        io.write(zeros, 16);
    }
}

static std::vector<char> readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read test audio input");
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

static void writeAll(const std::filesystem::path& p, const char* data, size_t n) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data, std::streamsize(n));
    out.flush();
    if (!out) throw std::runtime_error("cannot write test audio output");
}

void writeCorrupted(const std::filesystem::path& src, const std::filesystem::path& dst, double fraction) {
    if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) throw std::invalid_argument("invalid corruption fraction");
    auto b = readAll(src);
    size_t at = std::max<size_t>(8192, size_t(static_cast<double>(b.size()) * fraction));
    if (at < b.size()) b[at] = char(b[at] ^ 0x5A);
    writeAll(dst, b.data(), b.size());
}

void writeTruncated(const std::filesystem::path& src, const std::filesystem::path& dst, double fraction) {
    if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) throw std::invalid_argument("invalid truncation fraction");
    auto b = readAll(src);
    writeAll(dst, b.data(), size_t(static_cast<double>(b.size()) * fraction));
}

std::vector<std::filesystem::path> generateAll(const std::filesystem::path& dir, double seconds) {
    std::vector<std::filesystem::path> out;
    for (const auto& spec : standardMatrix(seconds)) {
        auto p = dir / (spec.name + ".flac");
        writeFlac(p, spec);
        out.push_back(p);
    }
    GenSpec noMd5{"no-md5-24-96k", 96000, 24, 2, seconds, false};
    out.push_back(dir / (noMd5.name + ".flac"));
    writeFlac(out.back(), noMd5);
    const auto base = dir / "24-96k.flac";
    out.push_back(dir / "damaged-24-96k.flac");
    writeCorrupted(base, out.back());
    out.push_back(dir / "truncated-24-96k.flac");
    writeTruncated(base, out.back());
    return out;
}

namespace {

struct RefState {
    ReferenceDecode* out;
    bool errorSeen = false;
};

FLAC__StreamDecoderWriteStatus onWrite(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[],
                                       void* user) {
    auto* st = static_cast<RefState*>(user);
    const unsigned ch = frame->header.channels, n = frame->header.blocksize;
    auto& v = st->out->samples;
    size_t at = v.size();
    v.resize(at + size_t(n) * ch);
    for (unsigned i = 0; i < n; i++)
        for (unsigned c = 0; c < ch; c++) v[at + size_t(i) * ch + c] = buffer[c][i];
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void onMetadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* user) {
    if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    auto& in = static_cast<RefState*>(user)->out->info;
    in.sampleRate = m->data.stream_info.sample_rate;
    in.channels = m->data.stream_info.channels;
    in.bitsPerSample = m->data.stream_info.bits_per_sample;
    in.totalFrames = m->data.stream_info.total_samples;
    std::array<uint8_t, 16> md5;
    std::copy(m->data.stream_info.md5sum, m->data.stream_info.md5sum + 16, md5.begin());
    if (std::any_of(md5.begin(), md5.end(), [](uint8_t x) { return x; })) in.md5 = md5;
    static_cast<RefState*>(user)->out->samples.reserve(size_t(in.totalFrames * in.channels));
}

void onError(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus s, void* user) {
    auto* st = static_cast<RefState*>(user);
    if (!st->errorSeen) st->out->error = FLAC__StreamDecoderErrorStatusString[s];
    st->errorSeen = true;
}

} // namespace

ReferenceDecode referenceDecode(const std::filesystem::path& path) {
    ReferenceDecode out;
    RefState st{&out};
    FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    FLAC__stream_decoder_set_md5_checking(dec, true);
    FILE* f = nullptr;
    if (_wfopen_s(&f, longPath(path).c_str(), L"rb") != 0 || !f) {
        FLAC__stream_decoder_delete(dec);
        out.error = "cannot open file";
        return out;
    }
    if (FLAC__stream_decoder_init_FILE(dec, f, onWrite, onMetadata, onError, &st) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        out.error = "decoder init failed";
        return out;
    }
    if (!FLAC__stream_decoder_process_until_end_of_stream(dec) && out.error.empty())
        out.error = FLAC__stream_decoder_get_resolved_state_string(dec);
    if (out.error.empty() && out.info.totalFrames && out.samples.size() / std::max(1u, out.info.channels) != out.info.totalFrames)
        out.error = "stream ended early";
    out.md5Checked = out.info.md5.has_value() && out.error.empty();
    out.md5Ok = FLAC__stream_decoder_finish(dec) != 0 && out.md5Checked; // finish() returns false on MD5 mismatch
    FLAC__stream_decoder_delete(dec);
    return out;
}

} // namespace camel::audio::testaudio
