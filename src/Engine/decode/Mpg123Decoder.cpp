// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through libmpg123 with gapless decoding: LAME/Xing encoder delay and padding are trimmed by the library.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#include <mpg123.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

// One libmpg123 stream of the file. Once `stop` is set its reads fail, which ends a scan no one waits for any more.
class Stream {
public:
    Stream(const std::filesystem::path& path, const std::atomic<bool>* stop = nullptr) : stop_(stop) {
        static std::once_flag once;
        std::call_once(once, [] { mpg123_init(); });
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, longPath(path).c_str(), L"rb") != 0 || !file)
            throw Error(ErrorCode::FileOpen, "mpg123: cannot open file");
        file_.reset(file);
        int error = MPG123_OK;
        handle_.reset(mpg123_new(nullptr, &error));
        if (!handle_) throw Error(ErrorCode::Other, std::string("mpg123_new: ") + mpg123_plain_strerror(error));
        mpg123_handle* mh = handle_.get();
        mpg123_param2(mh, MPG123_ADD_FLAGS, MPG123_GAPLESS | MPG123_QUIET | MPG123_FORCE_FLOAT, 0.0);
        // Float output keeps the decoder's precision until the single quantization in read().
        mpg123_format_none(mh);
        const long* rates = nullptr;
        size_t count = 0;
        mpg123_rates(&rates, &count);
        for (size_t i = 0; i < count; i++) mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);
        if (mpg123_reader64(mh, readFile, seekFile, nullptr) != MPG123_OK || mpg123_open_handle64(mh, this) != MPG123_OK)
            fail(ErrorCode::FileOpen, "mpg123_open_handle64");
        int encoding = 0;
        if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK || encoding != MPG123_ENC_FLOAT_32)
            fail(ErrorCode::UnsupportedStream, "mpg123_getformat");
        mpg123_format_none(mh); // the format must not change mid-stream
        mpg123_format(mh, rate, channels, encoding);
    }

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    mpg123_handle* mh() const { return handle_.get(); }

    [[noreturn]] void fail(ErrorCode code, const char* what) const {
        throw Error(code, std::string(what) + ": " + mpg123_strerror(handle_.get()));
    }

    long rate = 0;
    int channels = 0;

private:
    static int readFile(void* handle, void* buffer, size_t bytes, size_t* got) {
        auto* stream = static_cast<Stream*>(handle);
        *got = 0;
        if (stream->stop_ && stream->stop_->load(std::memory_order_relaxed)) return -1;
        *got = std::fread(buffer, 1, bytes, stream->file_.get());
        return std::ferror(stream->file_.get()) ? -1 : 0;
    }

    static int64_t seekFile(void* handle, int64_t offset, int origin) {
        std::FILE* file = static_cast<Stream*>(handle)->file_.get();
        return _fseeki64(file, offset, origin) == 0 ? _ftelli64(file) : -1;
    }

    const std::atomic<bool>* stop_;
    // The handle goes first, then the file it reads.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
    std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> handle_{nullptr, &mpg123_delete};
};

class Mpg123Decoder final : public IDecoder {
public:
    // `measure`: playback. The exact length and seek index cost a scan of the whole file, seconds from a network
    // share, so a second stream scans while this one plays; the length counts from when it is known, and seeks wait
    // for it. Otherwise (library scans) the length stays mpg123's estimate.
    Mpg123Decoder(const std::filesystem::path& path, uint32_t lossyBits, bool measure) : stream_(std::make_unique<Stream>(path)) {
        info_.sampleRate = uint32_t(stream_->rate);
        info_.channels = uint32_t(stream_->channels);
        info_.bitsPerSample = lossyBits;
        // mpg123 reads a Xing/Info header but not VBRI, whose VBR stream it would estimate from the first bitrate.
        const int64_t length = mpg123_length64(stream_->mh());
        const uint64_t vbri = vbriSamples(path);
        estimate_ = vbri ? vbri : length > 0 ? uint64_t(length) : 0;
        if (measure)
            scan_ = std::thread([this, path] { scanMain(path); });
        else
            info_.totalFrames = estimate_;
    }

    ~Mpg123Decoder() override {
        stop_.store(true, std::memory_order_relaxed);
        if (scan_.joinable()) scan_.join();
    }
    Mpg123Decoder(const Mpg123Decoder&) = delete;
    Mpg123Decoder& operator=(const Mpg123Decoder&) = delete;

    const SourceInfo& info() const override { return info_; }
    uint64_t estimatedFrames() const override { return info_.totalFrames ? info_.totalFrames : estimate_; }

    size_t read(int32_t* dst, size_t frames) override {
        buffer_.resize(frames * info_.channels);
        const size_t got = readFloat(buffer_.data(), frames);
        for (size_t i = 0, n = got * info_.channels; i < n; i++) dst[i] = quantize(buffer_[i], info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return true; }
    size_t readFloat(float* dst, size_t frames) override {
        if (!frames || ended_) return 0;
        if (scanDone_.load(std::memory_order_acquire)) finishScan();
        // The stream ends at the exact length even where its LAME frame claims more.
        if (info_.totalFrames)
            frames = position_ < info_.totalFrames ? size_t(std::min<uint64_t>(frames, info_.totalFrames - position_)) : 0;
        size_t bytes = 0;
        while (!bytes && !ended_ && frames) {
            const int result = mpg123_read(stream_->mh(), dst, frames * info_.channels * sizeof(float), &bytes);
            if (result == MPG123_DONE && !bytes) {
                // The unscanned stream may stop where its LAME frame says, short of the audio; the scan knows.
                if (continueScanned()) continue;
                ended_ = true;
            } else if (result != MPG123_OK && result != MPG123_DONE) {
                stream_->fail(ErrorCode::DecodeFailed, "mpg123_read");
            }
        }
        const size_t got = bytes / sizeof(float) / info_.channels;
        position_ += got;
        return got;
    }

    void seek(uint64_t frame) override {
        // The scanned stream's index lands a seek exactly without reading the file up to it a second time.
        finishScan(true);
        if (scanned_) stream_ = std::move(scanned_);
        if (mpg123_seek64(stream_->mh(), int64_t(frame), SEEK_SET) != int64_t(frame))
            stream_->fail(ErrorCode::SeekFailed, "mpg123_seek64");
        position_ = frame;
        ended_ = false;
    }

private:
    void scanMain(const std::filesystem::path& path) {
        try {
            auto scanned = std::make_unique<Stream>(path, &stop_);
            if (mpg123_scan(scanned->mh()) == MPG123_OK) scanned_ = std::move(scanned);
        } catch (const Error&) {
            // The playing stream goes on with its length unknown.
        }
        scanDone_.store(true, std::memory_order_release);
    }

    // Takes the scan's exact length, waiting for the scan when `wait` is set.
    void finishScan(bool wait = false) {
        if (!scan_.joinable() || (!wait && !scanDone_.load(std::memory_order_acquire))) return;
        scan_.join();
        if (!scanned_) return;
        const int64_t length = mpg123_length64(scanned_->mh());
        if (length > 0 && uint64_t(length) >= position_) info_.totalFrames = uint64_t(length);
    }

    // At the end of the unscanned stream: continues from the scanned one at the same frame, if it holds more.
    bool continueScanned() {
        finishScan(true);
        if (!scanned_ || position_ >= info_.totalFrames ||
            mpg123_seek64(scanned_->mh(), int64_t(position_), SEEK_SET) != int64_t(position_))
            return false;
        stream_ = std::move(scanned_);
        return true;
    }

    std::unique_ptr<Stream> stream_;
    std::unique_ptr<Stream> scanned_; // written by the scan thread before scanDone_; read after joining it
    std::atomic<bool> stop_{false};
    std::atomic<bool> scanDone_{false};
    std::thread scan_;
    SourceInfo info_;
    uint64_t estimate_ = 0;
    uint64_t position_ = 0;
    std::vector<float> buffer_;
    bool ended_ = false;
};

} // namespace

std::unique_ptr<IDecoder> openMpg123(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<Mpg123Decoder>(path, lossyBits, true);
}

SourceInfo probeMp3Info(const std::filesystem::path& path, bool& estimated) {
    estimated = !lossyLengthDeclared(path);
    const Mpg123Decoder decoder(path, 24, false);
    return decoder.info();
}

} // namespace camel::audio
