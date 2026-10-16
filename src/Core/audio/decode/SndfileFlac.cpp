// FLAC through libsndfile (which decodes FLAC with libFLAC internally).
#include "audio/decode/DecoderImpl.h"

#include <windows.h>
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#include <sndfile.h>

#include "util/Text.h"

namespace camel::audio {
namespace {

class SndfileFlac final : public IDecoder {
public:
    explicit SndfileFlac(const std::filesystem::path& path) {
        SF_INFO si{};
        sf_ = sf_wchar_open(longPath(path).c_str(), SFM_READ, &si);
        if (!sf_) throw Error(ErrorCode::FileOpen, std::string("sf_wchar_open: ") + sf_strerror(nullptr));
        switch (si.format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8:
            info_.bitsPerSample = 8;
            break;
        case SF_FORMAT_PCM_16:
            info_.bitsPerSample = 16;
            break;
        case SF_FORMAT_PCM_24:
            info_.bitsPerSample = 24;
            break;
        case SF_FORMAT_PCM_32:
            info_.bitsPerSample = 32;
            break;
        default:
            sf_close(sf_);
            throw Error(ErrorCode::UnsupportedStream, "libsndfile: unsupported sample format");
        }
        info_.sampleRate = uint32_t(si.samplerate);
        info_.channels = uint32_t(si.channels);
        info_.totalFrames = uint64_t(si.frames);
    }

    ~SndfileFlac() override { sf_close(sf_); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        sf_count_t got = sf_readf_int(sf_, dst, sf_count_t(frames));
        if (sf_error(sf_) != SF_ERR_NO_ERROR)
            throw Error(ErrorCode::DecodeFailed, std::string("libsndfile: ") + sf_strerror(sf_));
        // libsndfile scales integer reads to full 32-bit range; bring them back to the original bits.
        const int shift = 32 - int(info_.bitsPerSample);
        if (shift)
            for (size_t i = 0, n = size_t(got) * info_.channels; i < n; i++) dst[i] >>= shift;
        return size_t(got);
    }

    void seek(uint64_t frame) override {
        if (sf_seek(sf_, sf_count_t(frame), SEEK_SET) < 0)
            throw Error(ErrorCode::SeekFailed, std::string("sf_seek: ") + sf_strerror(sf_));
    }

private:
    SNDFILE* sf_ = nullptr;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openSndfileFlac(const std::filesystem::path& path) {
    return std::make_unique<SndfileFlac>(path);
}

} // namespace camel::audio
