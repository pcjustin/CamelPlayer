// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// FLAC, ALAC, WAV, AIFF, AAC and MP3 through Symphonia (Rust, rust/symphonia_ffi).
#include "Engine/decode/DecoderImpl.h"

#include <bit>
#include <cstring>
#include <vector>

#include "symphonia_ffi.h"
#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class SymphoniaDecoder final : public IDecoder {
public:
    SymphoniaDecoder(const std::filesystem::path& path, uint32_t lossyBits) {
        SymInfo si{};
        int32_t r = sym_open(toUtf8(longPath(path)).c_str(), lossyBits, &dec_, &si);
        if (r == SYM_E_IO) throw Error(ErrorCode::FileOpen, "symphonia: cannot open file");
        if (r == SYM_E_UNSUPPORTED) throw Error(ErrorCode::UnsupportedStream, "symphonia: unsupported stream");
        if (r != SYM_OK) throw Error(ErrorCode::UnsupportedStream, "symphonia: open failed (" + std::to_string(r) + ")");
        info_.sampleRate = si.sample_rate;
        info_.channels = si.channels;
        info_.bitsPerSample = si.bits_per_sample;
        info_.totalFrames = si.total_frames;
        float_ = si.is_float != 0;
        if (si.has_md5) {
            std::array<uint8_t, 16> m;
            std::copy(si.md5, si.md5 + 16, m.begin());
            info_.md5 = m;
        }
    }

    ~SymphoniaDecoder() override { sym_close(dec_); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        const size_t got = readRaw(dst, frames);
        if (float_)
            for (size_t i = 0, n = got * info_.channels; i < n; i++)
                dst[i] = quantize(std::bit_cast<float>(dst[i]), info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return float_; }
    size_t readFloat(float* dst, size_t frames) override {
        raw_.resize(frames * info_.channels);
        const size_t got = readRaw(raw_.data(), frames);
        std::memcpy(dst, raw_.data(), got * info_.channels * sizeof(float));
        return got;
    }

    void seek(uint64_t frame) override {
        if (sym_seek(dec_, frame) != SYM_OK) throw Error(ErrorCode::SeekFailed, "symphonia: " + lastError());
    }

private:
    // Integers, or float32 bit patterns for a lossy stream.
    size_t readRaw(int32_t* dst, size_t frames) {
        size_t got = 0;
        if (sym_read(dec_, dst, frames, &got) < 0) throw Error(ErrorCode::DecodeFailed, "symphonia: " + lastError());
        return got;
    }

    std::string lastError() const {
        char buf[512] = {};
        sym_last_error(dec_, buf, sizeof buf);
        return buf;
    }

    SymDecoder* dec_ = nullptr;
    SourceInfo info_;
    bool float_ = false;
    std::vector<int32_t> raw_;
};

} // namespace

std::unique_ptr<IDecoder> openSymphonia(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<SymphoniaDecoder>(path, lossyBits);
}

} // namespace camel::audio
