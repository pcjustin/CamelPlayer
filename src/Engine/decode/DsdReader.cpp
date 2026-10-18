#include "Engine/decode/DsdReader.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#define DSF_USING_DLL
#define DSDIFF_USING_DLL
#define DST_USING_DLL
#include <libdsdiff/dsdiff.h>
#include <libdsf/dsf.h>
extern "C" {
#include <libdst/decoder.h>
}

#include "Common/AudioTypes.h"
#include "Common/util/Text.h"
#include "Engine/decode/DecoderImpl.h"

namespace camel::audio {
namespace {

constexpr size_t kDsfBlockBytes = DSF_BLOCK_SIZE_PER_CHANNEL;

uint8_t reverseBits(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

void requireRate(uint32_t channels, uint32_t rate) {
    if (!channels || !rate || rate % (44100 * 64))
        throw Error(ErrorCode::UnsupportedStream, "DSD: unsupported channel count or rate");
}

class DsfReader final : public DsdReader {
public:
    explicit DsfReader(const std::string& path) {
        if (dsf_alloc(&dsf_) != DSF_SUCCESS) throw Error(ErrorCode::Other, "dsf_alloc failed");
        if (const int rc = dsf_open(dsf_, path.c_str()); rc != DSF_SUCCESS) {
            dsf_free(dsf_);
            throw Error(rc == DSF_ERROR_FILE_NOT_FOUND ? ErrorCode::FileOpen : ErrorCode::DecodeFailed,
                        std::string("dsf_open: ") + dsf_error_string(rc));
        }
        dsf_file_info_t fi{};
        dsf_get_file_info(dsf_, &fi);
        info_.channels = fi.channel_count;
        info_.sampleRate = fi.sampling_frequency;
        // libdsf returns the final block's padding too; only sample_count bits per channel are music.
        info_.frames = std::min<uint64_t>(fi.sample_count / 8, fi.audio_data_size / std::max(fi.channel_count, 1u));
        // libdsf always reverses bit order, which is right only for LSB-first (1-bit) files.
        msbFirst_ = fi.bits_per_sample == 8;
        try {
            requireRate(info_.channels, info_.sampleRate);
            // dsf_open leaves the file at the trailing ID3 metadata when there is one.
            if (dsf_seek_to_audio_start(dsf_) != DSF_SUCCESS)
                throw Error(ErrorCode::DecodeFailed, "dsf_seek_to_audio_start failed");
        } catch (...) {
            close();
            throw;
        }
    }

    ~DsfReader() override { close(); }
    DsfReader(const DsfReader&) = delete;
    DsfReader& operator=(const DsfReader&) = delete;

    const DsdStreamInfo& info() const override { return info_; }

    size_t read(uint8_t* dst, size_t frames) override {
        frames = size_t(std::min<uint64_t>(frames, info_.frames - pos_));
        if (!frames) return 0;
        const size_t want = frames * info_.channels;
        size_t got = 0;
        dsf_read_audio_data(dsf_, dst, want, &got);
        if (got != want)
            throw Error(ErrorCode::DecodeFailed, "DSF: audio data ends at frame " + std::to_string(pos_ + got / info_.channels));
        if (msbFirst_)
            for (size_t i = 0; i < want; i++) dst[i] = reverseBits(dst[i]);
        pos_ += frames;
        return frames;
    }

    void seek(uint64_t frame) override {
        if (frame > info_.frames) throw Error(ErrorCode::SeekFailed, "DSF: seek beyond end");
        const uint64_t block = frame / kDsfBlockBytes;
        if (dsf_seek_audio_data(dsf_, int64_t(block * kDsfBlockBytes * info_.channels), DSF_SEEK_SET) != DSF_SUCCESS)
            throw Error(ErrorCode::SeekFailed, "dsf_seek_audio_data failed");
        pos_ = block * kDsfBlockBytes;
        const auto skip = size_t(frame - pos_);
        std::vector<uint8_t> discard(skip * info_.channels);
        if (skip) read(discard.data(), skip);
    }

private:
    void close() {
        dsf_close(dsf_);
        dsf_free(dsf_);
    }

    dsf_t* dsf_ = nullptr;
    DsdStreamInfo info_;
    uint64_t pos_ = 0;
    bool msbFirst_ = false;
};

class DffReader final : public DsdReader {
public:
    DffReader(const std::string& path, bool allowDst) {
        if (dsdiff_new(&dff_) != DSDIFF_SUCCESS) throw Error(ErrorCode::Other, "dsdiff_new failed");
        // dsdiff_open frees the handle when it fails.
        if (const int rc = dsdiff_open(dff_, path.c_str()); rc != DSDIFF_SUCCESS) {
            throw Error(rc == DSDIFF_ERROR_FILE_NOT_FOUND ? ErrorCode::FileOpen : ErrorCode::DecodeFailed,
                        "dsdiff_open failed (" + std::to_string(rc) + ")");
        }
        try {
            init(allowDst);
        } catch (...) {
            close();
            throw;
        }
    }

    ~DffReader() override { close(); }
    DffReader(const DffReader&) = delete;
    DffReader& operator=(const DffReader&) = delete;

    const DsdStreamInfo& info() const override { return info_; }

    size_t read(uint8_t* dst, size_t frames) override {
        frames = size_t(std::min<uint64_t>(frames, info_.frames - pos_));
        if (!frames) return 0;
        if (info_.dst) return readDst(dst, frames);
        const auto want = uint32_t(frames * info_.channels);
        uint32_t got = 0;
        dsdiff_read_dsd_data(dff_, dst, want, &got);
        if (got != want)
            throw Error(ErrorCode::DecodeFailed,
                        "DSDIFF: audio data ends at frame " + std::to_string(pos_ + got / info_.channels));
        pos_ += frames;
        return frames;
    }

    void seek(uint64_t frame) override {
        if (frame > info_.frames) throw Error(ErrorCode::SeekFailed, "DSDIFF: seek beyond end");
        if (!info_.dst) {
            if (dsdiff_seek_dsd_data(dff_, int64_t(frame), DSDIFF_SEEK_SET) != DSDIFF_SUCCESS)
                throw Error(ErrorCode::SeekFailed, "dsdiff_seek_dsd_data failed");
            pos_ = frame;
            return;
        }
        if (dsdiff_seek_dsd_start(dff_) != DSDIFF_SUCCESS) throw Error(ErrorCode::SeekFailed, "dsdiff_seek_dsd_start failed");
        pos_ = 0;
        pending_.clear();
        nextDstFrame_ = 0;
        std::vector<uint8_t> skip(dstFrameBytes_);
        while (pos_ < frame) read(skip.data(), size_t(std::min<uint64_t>(frame - pos_, dstFrameBytes_ / info_.channels)));
    }

private:
    void init(bool allowDst) {
        uint16_t channels = 0;
        uint16_t bits = 0;
        dsdiff_audio_type_t type{};
        dsdiff_get_channel_count(dff_, &channels);
        dsdiff_get_sample_bits(dff_, &bits);
        dsdiff_get_sample_rate(dff_, &info_.sampleRate);
        dsdiff_get_audio_type(dff_, &type);
        info_.channels = channels;
        requireRate(info_.channels, info_.sampleRate);
        if (bits != 1) throw Error(ErrorCode::UnsupportedStream, "DSDIFF: not 1-bit DSD");
        if (type == DSDIFF_AUDIO_DSD) {
            dsdiff_get_sample_frame_count(dff_, &info_.frames);
            return;
        }
        if (type != DSDIFF_AUDIO_DST || !allowDst) throw Error(ErrorCode::UnsupportedStream, "DSDIFF: DST is not enabled");
        info_.dst = true;
        uint16_t frameRate = 0;
        dsdiff_get_dst_frame_count(dff_, &dstFrames_);
        dsdiff_get_dst_frame_rate(dff_, &frameRate);
        if (frameRate != 75) throw Error(ErrorCode::UnsupportedStream, "DST: frame rate is not 75");
        dstFrameBytes_ = size_t(info_.sampleRate / 8 / 75) * info_.channels;
        info_.frames = uint64_t(dstFrames_) * (dstFrameBytes_ / info_.channels);
        if (dst_decoder_init(&dst_, int(info_.channels), int(info_.sampleRate)) != 0)
            throw Error(ErrorCode::UnsupportedStream, "dst_decoder_init failed");
    }

    size_t readDst(uint8_t* dst, size_t frames) {
        size_t done = 0;
        while (done < frames) {
            if (pending_.empty()) decodeDstFrame();
            const size_t n = std::min(frames - done, (pending_.size() - pendingPos_) / info_.channels);
            std::copy_n(pending_.begin() + ptrdiff_t(pendingPos_), n * info_.channels, dst + done * info_.channels);
            pendingPos_ += n * info_.channels;
            done += n;
            if (pendingPos_ == pending_.size()) pending_.clear();
        }
        pos_ += frames;
        return frames;
    }

    void decodeDstFrame() {
        if (nextDstFrame_ >= dstFrames_) throw Error(ErrorCode::DecodeFailed, "DST: frame count exceeded");
        // An uncoded frame is one header byte plus the raw frame; a coded frame is never larger.
        frame_.assign(dstFrameBytes_ + 1, 0);
        uint32_t size = 0;
        if (dsdiff_read_dst_frame(dff_, frame_.data(), uint32_t(frame_.size()), &size) != DSDIFF_SUCCESS || size == 0)
            throw Error(ErrorCode::DecodeFailed, "DST: cannot read frame " + std::to_string(nextDstFrame_));
        if ((frame_[0] & 0x80) == 0 && size != frame_.size())
            throw Error(ErrorCode::DecodeFailed, "DST: uncoded frame " + std::to_string(nextDstFrame_) + " has the wrong size");
        pending_.assign(dstFrameBytes_, 0);
        int outLen = 0;
        if (dst_decoder_decode(dst_, frame_.data(), int(size), pending_.data(), &outLen) != 0)
            throw Error(ErrorCode::DecodeFailed, "DST: frame " + std::to_string(nextDstFrame_) + " is corrupt");
        pendingPos_ = 0;
        nextDstFrame_++;
    }

    void close() {
        if (dst_) dst_decoder_close(dst_);
        dsdiff_close(dff_);
    }

    dsdiff_t* dff_ = nullptr;
    dst_decoder_t* dst_ = nullptr;
    DsdStreamInfo info_;
    uint64_t pos_ = 0;
    uint32_t dstFrames_ = 0;
    uint32_t nextDstFrame_ = 0;
    size_t dstFrameBytes_ = 0;
    std::vector<uint8_t> frame_, pending_;
    size_t pendingPos_ = 0;
};

class DsdDecoder final : public IDecoder {
public:
    explicit DsdDecoder(std::unique_ptr<DsdReader> reader) : reader_(std::move(reader)) {
        const auto& dsd = reader_->info();
        info_.sampleRate = dsd.sampleRate / 8;
        info_.channels = dsd.channels;
        info_.bitsPerSample = 1;
        info_.totalFrames = dsd.frames;
    }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        bytes_.resize(frames * info_.channels);
        const size_t got = reader_->read(bytes_.data(), frames);
        std::copy_n(bytes_.begin(), got * info_.channels, dst);
        return got;
    }

    void seek(uint64_t frame) override { reader_->seek(frame); }

private:
    std::unique_ptr<DsdReader> reader_;
    SourceInfo info_;
    std::vector<uint8_t> bytes_;
};

} // namespace

std::unique_ptr<IDecoder> openDsdNexus(const std::filesystem::path& path) {
    return std::make_unique<DsdDecoder>(openDsdReader(path));
}

std::unique_ptr<DsdReader> openDsdReader(const std::filesystem::path& path, bool allowDst) {
    const std::wstring wide = longPath(path);
    std::ifstream in(std::filesystem::path(wide), std::ios::binary);
    if (!in) throw Error(ErrorCode::FileOpen, "cannot open file");
    std::string tag(4, '\0');
    in.read(tag.data(), 4);
    if (tag == "DSD ") return std::make_unique<DsfReader>(toUtf8(wide));
    if (tag == "FRM8") return std::make_unique<DffReader>(toUtf8(wide), allowDst);
    throw Error(ErrorCode::UnsupportedStream, "not a DSF or DSDIFF file");
}

} // namespace camel::audio
