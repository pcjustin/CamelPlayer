// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/decode/DecoderImpl.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "Common/DecoderRegistry.h"

#include "Engine/decode/FlacStreamInfo.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/verify/FlacMd5.h"

namespace camel::audio {

const char* decoderName(DecoderKind k) {
    switch (k) {
    case DecoderKind::Symphonia:
        return "Symphonia";
    case DecoderKind::DrFlac:
        return "dr_flac";
    case DecoderKind::Sndfile:
        return "libsndfile";
    case DecoderKind::DrWav:
        return "dr_wav";
    case DecoderKind::Mpg123:
        return "mpg123";
    case DecoderKind::DsdNexus:
        return "DSD Nexus";
    }
    return "?";
}

const char* decoderId(DecoderKind k) {
    switch (k) {
    case DecoderKind::Symphonia:
        return "symphonia";
    case DecoderKind::DrFlac:
        return "drflac";
    case DecoderKind::Sndfile:
        return "sndfile";
    case DecoderKind::DrWav:
        return "drwav";
    case DecoderKind::Mpg123:
        return "mpg123";
    case DecoderKind::DsdNexus:
        return "dsdnexus";
    }
    return "?";
}

bool decoderFromId(std::string_view id, DecoderKind& out) {
    for (DecoderKind k : kAllDecoders)
        if (id == decoderId(k)) {
            out = k;
            return true;
        }
    return false;
}

namespace {

// Gives every decoder the same STREAMINFO MD5 (from our own parser), so the engine and reports
// do not depend on which decoder happens to expose it.
class WithStreamInfo final : public IDecoder {
public:
    WithStreamInfo(std::unique_ptr<IDecoder> inner, const SourceInfo& parsed) : inner_(std::move(inner)), info_(inner_->info()) {
        info_.md5 = parsed.md5;
        if (!info_.totalFrames) info_.totalFrames = parsed.totalFrames;
        if (info_.md5) md5_ = std::make_unique<FlacMd5>(info_.bitsPerSample);
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override {
        if (!frames || ended_) return 0;
        const size_t got = inner_->read(dst, frames);
        position_ += got;
        if (info_.totalFrames && (position_ > info_.totalFrames || (!got && position_ != info_.totalFrames)))
            throw Error(ErrorCode::DecodeFailed, "FLAC frame count does not match STREAMINFO");
        if (got) {
            if (md5_) md5_->update(dst, got * info_.channels);
        } else {
            ended_ = true;
            if (md5_ && info_.md5 && md5_->finish() != *info_.md5) throw Error(ErrorCode::DecodeFailed, "FLAC MD5 mismatch");
        }
        return got;
    }
    void seek(uint64_t frame) override {
        if (info_.totalFrames && frame >= info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past FLAC end");
        inner_->seek(frame);
        position_ = frame;
        ended_ = false;
        md5_.reset();
        if (!frame && info_.md5) md5_ = std::make_unique<FlacMd5>(info_.bitsPerSample);
    }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t position_ = 0;
    bool ended_ = false;
    std::unique_ptr<FlacMd5> md5_;
};

// Stops at the container's frame count. Symphonia 0.5.5 counts the AIFF SSND offset and block-size words as audio, so
// it reports one or two extra frames and would decode bytes of a chunk that follows the sound data.
class FrameLimit final : public IDecoder {
public:
    FrameLimit(std::unique_ptr<IDecoder> inner, uint64_t frames) : inner_(std::move(inner)), info_(inner_->info()) {
        info_.totalFrames = frames;
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override {
        const size_t got = inner_->read(dst, size_t(std::min<uint64_t>(frames, info_.totalFrames - position_)));
        position_ += got;
        return got;
    }
    void seek(uint64_t frame) override {
        if (frame >= info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past the end");
        inner_->seek(frame);
        position_ = frame;
    }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t position_ = 0;
};

// Lossy decoders need a few frames of warm-up after a seek (the AAC MDCT overlap), and some land beside the target when
// they also trim encoder delay. This seeks early, or reopens the file near the start, and decodes and discards up to the target,
// so a seek matches the linear decode exactly (docs/Plan.md §16.3).
std::unique_ptr<IDecoder> openAdapter(DecoderKind kind, const std::filesystem::path& path, uint32_t lossyBits);

class LossySeek final : public IDecoder {
public:
    static constexpr uint64_t kPreroll = 4608; // four and a half AAC frames

    // Without `lengthKnown` the decoder's length is only an estimate, as a header can count frames the decoder does not
    // decode: it is reported as unknown (0), since the Engine treats a nonzero length as exact, and becomes the frames
    // decoded once a read reaches the end.
    LossySeek(DecoderKind kind, std::filesystem::path path, uint32_t lossyBits, bool lengthKnown)
        : kind_(kind), path_(std::move(path)), lossyBits_(lossyBits), inner_(openAdapter(kind_, path_, lossyBits_)),
          info_(inner_->info()) {
        if (lengthKnown) return;
        estimate_ = info_.totalFrames;
        info_.totalFrames = 0;
    }
    const SourceInfo& info() const override { return info_; }
    uint64_t estimatedFrames() const override { return estimate_; }
    size_t read(int32_t* dst, size_t frames) override { return counted(inner_->read(dst, frames), frames); }
    bool readsFloat() const override { return inner_->readsFloat(); }
    size_t readFloat(float* dst, size_t frames) override { return counted(inner_->readFloat(dst, frames), frames); }
    void seek(uint64_t frame) override {
        const uint64_t start = frame > kPreroll ? frame - kPreroll : 0;
        if (start)
            inner_->seek(start);
        else
            inner_ = openAdapter(kind_, path_, lossyBits_);
        std::vector<int32_t> scratch(size_t(4096) * info_.channels);
        for (uint64_t left = frame - start; left;) {
            const size_t got = inner_->read(scratch.data(), size_t(std::min<uint64_t>(left, 4096)));
            if (!got) throw Error(ErrorCode::SeekFailed, "seek target is past the end");
            left -= got;
        }
        position_ = frame;
    }

private:
    size_t counted(size_t got, size_t asked) {
        position_ += got;
        if (!got && asked && !info_.totalFrames) info_.totalFrames = position_;
        return got;
    }

    DecoderKind kind_;
    std::filesystem::path path_;
    uint32_t lossyBits_;
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t estimate_ = 0;
    uint64_t position_ = 0;
};

// Removes the encoder delay and padding an MP4 declares, for a decoder that does not (Symphonia 0.5.5 ignores both the
// edit list and iTunSMPB). The inner decoder must seek exactly.
class TrimDelay final : public IDecoder {
public:
    TrimDelay(std::unique_ptr<IDecoder> inner, const EncoderTrim& trim)
        : inner_(std::move(inner)), info_(inner_->info()), priming_(trim.priming) {
        info_.totalFrames = trim.frames;
        if (priming_) inner_->seek(priming_);
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override {
        const size_t got = inner_->read(dst, size_t(std::min<uint64_t>(frames, info_.totalFrames - position_)));
        position_ += got;
        return got;
    }
    bool readsFloat() const override { return inner_->readsFloat(); }
    size_t readFloat(float* dst, size_t frames) override {
        const size_t got = inner_->readFloat(dst, size_t(std::min<uint64_t>(frames, info_.totalFrames - position_)));
        position_ += got;
        return got;
    }
    void seek(uint64_t frame) override {
        if (frame >= info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past the end");
        inner_->seek(priming_ + frame);
        position_ = frame;
    }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t priming_;
    uint64_t position_ = 0;
};

} // namespace

size_t IDecoder::readFloat(float*, size_t) {
    throw Error(ErrorCode::UnsupportedStream, "this decoder has no float output");
}

namespace {

std::unique_ptr<IDecoder> openAdapter(DecoderKind kind, const std::filesystem::path& path, uint32_t lossyBits) {
    switch (kind) {
    case DecoderKind::Symphonia:
        return openSymphonia(path, lossyBits);
    case DecoderKind::DrFlac:
        return openDrFlac(path);
    case DecoderKind::Sndfile:
        return openSndfile(path);
    case DecoderKind::DrWav:
        return openDrWav(path);
    case DecoderKind::Mpg123:
        return openMpg123(path, lossyBits);
    case DecoderKind::DsdNexus:
        return openDsdNexus(path);
    }
    throw Error(ErrorCode::UnsupportedStream, "unknown decoder selection");
}

} // namespace

namespace {

// A built-in decoder with the corrections it needs for `format` (docs/Plan.md §3.1).
std::unique_ptr<IDecoder> openBuiltin(DecoderKind kind, const std::filesystem::path& path, AudioFormat format,
                                      uint32_t lossyBits) {
    if (format == AudioFormat::Dsd) return openAdapter(kind, path, 0); // raw DSD, not PCM
    // ADPCM decodes to exact 16-bit samples, which libsndfile seeks exactly.
    if (format == AudioFormat::Adpcm) return openAdapter(kind, path, lossyBits);
    // The WAVE decoders read the bitstream as PCM, which the DTS stage decodes and seeks exactly. One sample per word
    // frame makes the data chunk's length exact.
    if (format == AudioFormat::Dts) return openDtsStage(openAdapter(kind, path, lossyBits), lossyBits);
    std::unique_ptr<IDecoder> d;
    // mpg123 seeks an MP3 exactly by itself; Symphonia lands on an AAC target after warm-up.
    if (isLossy(format) && kind != DecoderKind::Mpg123)
        d = std::make_unique<LossySeek>(kind, path, lossyBits, lossyLengthDeclared(path));
    else
        d = openAdapter(kind, path, lossyBits);
    if (format == AudioFormat::Aac && kind == DecoderKind::Symphonia)
        if (const auto trim = mp4EncoderTrim(path, d->info().sampleRate)) d = std::make_unique<TrimDelay>(std::move(d), *trim);
    if (format == AudioFormat::Aiff && kind == DecoderKind::Symphonia)
        if (const auto frames = aiffSampleFrames(path)) d = std::make_unique<FrameLimit>(std::move(d), *frames);
    return d;
}

void registerBuiltinOpeners() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const DecoderKind kind : kAllDecoders)
            decoderRegistry().setOpener(decoderId(kind),
                                        [kind](const std::filesystem::path& path, std::string_view id, uint32_t lossyBits) {
                                            AudioFormat format{};
                                            formatFromId(id, format);
                                            return openBuiltin(kind, path, format, lossyBits);
                                        });
    });
}

} // namespace

std::unique_ptr<IDecoder> openDecoder(std::string_view decoder, const std::filesystem::path& path) {
    // A lossy stream has no source depth; it decodes to 24-bit PCM (docs/Plan.md §3.1).
    constexpr uint32_t kLossyBits = 24;
    registerBuiltinOpeners();
    const AudioFormat format = probeFormat(path);
    const std::string_view id = formatId(format);
    const auto entry = decoderRegistry().find(decoder);
    if (!entry || !entry->open || std::find(entry->formats.begin(), entry->formats.end(), id) == entry->formats.end())
        throw Error(ErrorCode::UnsupportedStream,
                    (entry ? entry->name : std::string(decoder)) + " cannot decode " + formatName(format));
    auto d = entry->open(path, id, format == AudioFormat::Dsd ? 0 : kLossyBits);
    if (format == AudioFormat::Dsd) return d;
    // The player supports 16/24-bit integer PCM sources only (docs/Plan.md §3.1).
    if (const auto bits = d->info().bitsPerSample; bits != 16 && bits != 24)
        throw Error(ErrorCode::UnsupportedStream, std::to_string(bits) + "-bit sources are not supported (16/24-bit only)");
    SourceInfo parsed;
    if (format == AudioFormat::Flac && readFlacStreamInfo(path, parsed))
        return std::make_unique<WithStreamInfo>(std::move(d), parsed);
    return d;
}

std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path) {
    return openDecoder(decoderId(kind), path);
}

} // namespace camel::audio
