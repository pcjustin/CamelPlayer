// FLAC, ALAC, WAV and AIFF through Symphonia (Rust, rust/symphonia_ffi).
#include "Engine/decode/DecoderImpl.h"

#include "symphonia_ffi.h"
#include "Common/util/Text.h"

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
        if (si.has_md5) {
            std::array<uint8_t, 16> m;
            std::copy(si.md5, si.md5 + 16, m.begin());
            info_.md5 = m;
        }
    }

    ~SymphoniaDecoder() override { sym_close(dec_); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        size_t got = 0;
        int32_t r = sym_read(dec_, dst, frames, &got);
        if (r < 0) throw Error(ErrorCode::DecodeFailed, "symphonia: " + lastError());
        return got;
    }

    void seek(uint64_t frame) override {
        if (sym_seek(dec_, frame) != SYM_OK) throw Error(ErrorCode::SeekFailed, "symphonia: " + lastError());
    }

private:
    std::string lastError() const {
        char buf[512] = {};
        sym_last_error(dec_, buf, sizeof buf);
        return buf;
    }

    SymDecoder* dec_ = nullptr;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openSymphonia(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<SymphoniaDecoder>(path, lossyBits);
}

} // namespace camel::audio
