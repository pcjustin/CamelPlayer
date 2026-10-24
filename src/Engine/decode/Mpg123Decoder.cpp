// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through libmpg123 with gapless decoding: LAME/Xing encoder delay and padding are trimmed by the library.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#include <mpg123.h>

#include <cstdio>
#include <mutex>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class Mpg123Decoder final : public IDecoder {
public:
    Mpg123Decoder(const std::filesystem::path& path, uint32_t lossyBits, bool scan = true) {
        static std::once_flag once;
        std::call_once(once, [] { mpg123_init(); });
        int error = MPG123_OK;
        mh_ = mpg123_new(nullptr, &error);
        if (!mh_) throw Error(ErrorCode::Other, std::string("mpg123_new: ") + mpg123_plain_strerror(error));
        mpg123_param2(mh_, MPG123_ADD_FLAGS, MPG123_GAPLESS | MPG123_QUIET | MPG123_FORCE_FLOAT, 0.0);
        // Float output keeps the decoder's precision until the single quantization in read().
        mpg123_format_none(mh_);
        const long* rates = nullptr;
        size_t count = 0;
        mpg123_rates(&rates, &count);
        for (size_t i = 0; i < count; i++) mpg123_format(mh_, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);
        if (mpg123_open64(mh_, toUtf8(std::filesystem::absolute(path).wstring()).c_str()) != MPG123_OK)
            fail(ErrorCode::FileOpen, "mpg123_open64");
        long rate = 0;
        int channels = 0, encoding = 0;
        if (mpg123_getformat(mh_, &rate, &channels, &encoding) != MPG123_OK || encoding != MPG123_ENC_FLOAT_32)
            fail(ErrorCode::UnsupportedStream, "mpg123_getformat");
        mpg123_format_none(mh_); // the format must not change mid-stream
        mpg123_format(mh_, rate, channels, encoding);
        if (scan && mpg123_scan(mh_) != MPG123_OK) fail(ErrorCode::DecodeFailed, "mpg123_scan"); // exact length and seek index
        const int64_t length = mpg123_length64(mh_);
        info_.sampleRate = uint32_t(rate);
        info_.channels = uint32_t(channels);
        info_.bitsPerSample = lossyBits;
        info_.totalFrames = length > 0 ? uint64_t(length) : 0;
    }

    ~Mpg123Decoder() override {
        mpg123_close(mh_);
        mpg123_delete(mh_);
    }
    Mpg123Decoder(const Mpg123Decoder&) = delete;
    Mpg123Decoder& operator=(const Mpg123Decoder&) = delete;

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        buffer_.resize(frames * info_.channels);
        const size_t got = readFloat(buffer_.data(), frames);
        for (size_t i = 0, n = got * info_.channels; i < n; i++) dst[i] = quantize(buffer_[i], info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return true; }
    size_t readFloat(float* dst, size_t frames) override {
        if (!frames || ended_) return 0;
        size_t bytes = 0;
        while (!bytes && !ended_) {
            const int result = mpg123_read(mh_, dst, frames * info_.channels * sizeof(float), &bytes);
            if (result == MPG123_DONE)
                ended_ = true;
            else if (result != MPG123_OK)
                throw Error(ErrorCode::DecodeFailed, std::string("mpg123_read: ") + mpg123_strerror(mh_));
        }
        return bytes / sizeof(float) / info_.channels;
    }

    void seek(uint64_t frame) override {
        if (mpg123_seek64(mh_, int64_t(frame), SEEK_SET) != int64_t(frame))
            throw Error(ErrorCode::SeekFailed, std::string("mpg123_seek64: ") + mpg123_strerror(mh_));
        ended_ = false;
    }

private:
    [[noreturn]] void fail(ErrorCode code, const char* what) {
        const std::string detail = std::string(what) + ": " + mpg123_strerror(mh_);
        mpg123_close(mh_);
        mpg123_delete(mh_);
        throw Error(code, detail);
    }

    mpg123_handle* mh_ = nullptr;
    SourceInfo info_;
    std::vector<float> buffer_;
    bool ended_ = false;
};

} // namespace

std::unique_ptr<IDecoder> openMpg123(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<Mpg123Decoder>(path, lossyBits);
}

SourceInfo probeMp3Info(const std::filesystem::path& path, bool& estimated) {
    estimated = !lossyLengthDeclared(path);
    const Mpg123Decoder decoder(path, 24, false);
    return decoder.info();
}

} // namespace camel::audio
