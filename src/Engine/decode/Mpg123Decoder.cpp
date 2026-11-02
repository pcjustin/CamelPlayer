// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through libmpg123 with gapless decoding: LAME/Xing encoder delay and padding are trimmed by the library.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#include <mpg123.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

// Holds a scanning stream at its next read, so another thread can read its seek index; `stop` fails its reads, which
// ends a scan no one waits for any more.
struct ScanGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool pauseWanted = false;
    bool paused = false;
    bool stop = false;
};

// One libmpg123 stream of the file, which mpg123 sees from byte `start` on.
class Stream {
public:
    Stream(const std::filesystem::path& path, uint64_t start, ScanGate* gate = nullptr) : start_(start), gate_(gate) {
        static std::once_flag once;
        std::call_once(once, [] { mpg123_init(); });
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, longPath(path).c_str(), L"rb") != 0 || !file)
            throw Error(ErrorCode::FileOpen, "mpg123: cannot open file");
        file_.reset(file);
        if (_fseeki64(file, int64_t(start_), SEEK_SET) != 0) throw Error(ErrorCode::FileOpen, "mpg123: cannot seek");
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
    bool atEnd() const { return std::feof(file_.get()) != 0; }

    [[noreturn]] void fail(ErrorCode code, const char* what) const {
        throw Error(code, std::string(what) + ": " + mpg123_strerror(handle_.get()));
    }

    long rate = 0;
    int channels = 0;

private:
    static int readFile(void* handle, void* buffer, size_t bytes, size_t* got) {
        auto* stream = static_cast<Stream*>(handle);
        *got = 0;
        if (ScanGate* gate = stream->gate_) {
            std::unique_lock lock(gate->mutex);
            if (gate->pauseWanted) {
                gate->paused = true;
                gate->changed.notify_all();
                gate->changed.wait(lock, [gate] { return !gate->pauseWanted || gate->stop; });
                gate->paused = false;
            }
            if (gate->stop) return -1;
        }
        *got = std::fread(buffer, 1, bytes, stream->file_.get());
        return std::ferror(stream->file_.get()) ? -1 : 0;
    }

    static int64_t seekFile(void* handle, int64_t offset, int origin) {
        auto* stream = static_cast<Stream*>(handle);
        std::FILE* file = stream->file_.get();
        const int64_t start = int64_t(stream->start_);
        if (_fseeki64(file, origin == SEEK_SET ? start + offset : offset, origin) != 0) return -1;
        const int64_t at = _ftelli64(file) - start;
        return at >= 0 ? at : -1;
    }

    uint64_t start_;
    ScanGate* gate_;
    // The handle goes first, then the file it reads.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
    std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> handle_{nullptr, &mpg123_delete};
};

class Mpg123Decoder final : public IDecoder {
public:
    // `measure`: playback. The exact length and seek index cost a scan of the whole file, seconds from a network
    // share, so a second stream scans while this one plays; the length counts from when it is known, and a seek waits
    // until the scan has passed its target. Otherwise (library scans) the length stays mpg123's estimate.
    // mpg123 would decode a VBRI frame, which holds no sound, so it sees the file from the frame after it.
    Mpg123Decoder(const std::filesystem::path& path, uint32_t lossyBits, bool measure)
        : start_(audioAfterVbriFrame(path)), stream_(std::make_unique<Stream>(path, start_)) {
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
        {
            const std::scoped_lock lock(gate_.mutex);
            gate_.stop = true;
        }
        gate_.changed.notify_all();
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
            // A last frame that the end of the file, or an ID3v1 tag, cuts short ends the stream.
            const bool cutShort = result == MPG123_ERR && mpg123_errcode(stream_->mh()) == MPG123_ERR_READER && stream_->atEnd();
            if ((result == MPG123_DONE || cutShort) && !bytes) {
                // The unscanned stream may stop where its LAME frame says, short of the audio; the scan knows.
                if (continueScanned()) continue;
                // The scan counted the cut frame: it plays as silence, so the length holds.
                if (cutShort && position_ < info_.totalFrames) {
                    const size_t silent = size_t(std::min<uint64_t>(frames, info_.totalFrames - position_));
                    std::fill_n(dst, silent * info_.channels, 0.0f);
                    position_ += silent;
                    return silent;
                }
                ended_ = true;
            } else if (result != MPG123_OK && result != MPG123_DONE && !(cutShort && bytes)) {
                stream_->fail(ErrorCode::DecodeFailed, "mpg123_read");
            }
        }
        const size_t got = bytes / sizeof(float) / info_.channels;
        position_ += got;
        return got;
    }

    void seek(uint64_t frame) override {
        // The scan's index lands a seek exactly without reading the file up to it a second time: lent while the scan
        // runs, or with the scanned stream once it has ended.
        if (!lendScanIndex(frame)) {
            finishScan(true);
            if (scanned_) stream_ = std::move(scanned_);
        }
        if (mpg123_seek64(stream_->mh(), int64_t(frame), SEEK_SET) != int64_t(frame))
            stream_->fail(ErrorCode::SeekFailed, "mpg123_seek64");
        position_ = frame;
        ended_ = false;
    }

private:
    void scanMain(const std::filesystem::path& path) {
        try {
            auto scanning = std::make_unique<Stream>(path, start_, &gate_);
            {
                const std::scoped_lock lock(gate_.mutex);
                scanning_ = scanning.get();
            }
            const bool scanned = mpg123_scan(scanning->mh()) == MPG123_OK;
            {
                const std::scoped_lock lock(gate_.mutex);
                scanning_ = nullptr;
            }
            if (scanned) scanned_ = std::move(scanning);
        } catch (const Error&) {
            // The playing stream goes on with its length unknown.
        }
        {
            const std::scoped_lock lock(gate_.mutex);
            scanDone_.store(true, std::memory_order_release);
        }
        gate_.changed.notify_all();
    }

    // While the scan runs: waits until its index covers `frame`, then gives the playing stream a copy. False once the
    // scan has ended, whose stream then serves the seek.
    bool lendScanIndex(uint64_t frame) {
        const int64_t samplesPerFrame = mpg123_spf(stream_->mh());
        if (samplesPerFrame <= 0) return false;
        // Past the target by a few frames for the encoder delay and the frames decoded before it.
        const uint64_t needed = frame / uint64_t(samplesPerFrame) + 8;
        while (!scanDone_.load(std::memory_order_acquire)) {
            std::vector<int64_t> offsets;
            int64_t step = 0;
            {
                std::unique_lock lock(gate_.mutex);
                gate_.pauseWanted = true;
                gate_.changed.wait(lock, [this] { return gate_.paused || scanDone_.load(std::memory_order_acquire); });
                int64_t* index = nullptr;
                size_t fill = 0;
                if (gate_.paused && scanning_ && mpg123_index64(scanning_->mh(), &index, &step, &fill) == MPG123_OK)
                    offsets.assign(index, index + fill);
                gate_.pauseWanted = false;
            }
            gate_.changed.notify_all();
            if (step > 0 && offsets.size() > 1 && uint64_t(offsets.size() - 1) * uint64_t(step) >= needed)
                return mpg123_set_index64(stream_->mh(), offsets.data(), step, offsets.size()) == MPG123_OK;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
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

    uint64_t start_;
    std::unique_ptr<Stream> stream_;
    std::unique_ptr<Stream> scanned_; // written by the scan thread before scanDone_; read after joining it
    ScanGate gate_;
    Stream* scanning_ = nullptr; // the scan's stream while it scans, guarded by gate_.mutex
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
