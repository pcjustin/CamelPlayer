// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through libmpg123 with gapless decoding: LAME/Xing encoder delay and padding are trimmed by the library.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#include <mpg123.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
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

// The length the library knows for an MP3 (IDecoder.h, rememberLibraryLength).
struct LibraryLength {
    uint64_t frames = 0;
    bool estimated = false;
};

std::mutex libraryLengthsMutex;
std::vector<std::pair<std::wstring, LibraryLength>> libraryLengths; // the latest last

std::wstring libraryLengthKey(const std::filesystem::path& path) {
    return path.lexically_normal().wstring();
}

std::optional<LibraryLength> libraryLength(const std::filesystem::path& path) {
    const std::wstring key = libraryLengthKey(path);
    const std::scoped_lock lock(libraryLengthsMutex);
    for (const auto& [stored, length] : std::views::reverse(libraryLengths))
        if (_wcsicmp(stored.c_str(), key.c_str()) == 0) return length;
    return std::nullopt;
}

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
        if (_fseeki64(file, 0, SEEK_END) == 0) {
            const int64_t size = _ftelli64(file);
            char tag[3] = {};
            const bool id3v1 = size >= 128 && _fseeki64(file, size - 128, SEEK_SET) == 0 && std::fread(tag, 1, 3, file) == 3 &&
                               std::memcmp(tag, "TAG", 3) == 0;
            const int64_t end = size - (id3v1 ? 128 : 0);
            audioEnd_ = std::cmp_greater(end, start_) ? uint64_t(end - int64_t(start_)) : 0;
        }
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
    // Where the audio ends, from `start` on: before an ID3v1 tag.
    uint64_t audioEnd() const { return audioEnd_; }

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
        const auto start = int64_t(stream->start_);
        if (_fseeki64(file, origin == SEEK_SET ? start + offset : offset, origin) != 0) return -1;
        const int64_t at = _ftelli64(file) - start;
        return at >= 0 ? at : -1;
    }

    uint64_t start_;
    uint64_t audioEnd_ = 0;
    ScanGate* gate_;
    // The handle goes first, then the file it reads.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
    std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> handle_{nullptr, &mpg123_delete};
};

class Mpg123Decoder final : public IDecoder {
public:
    // `measure`: playback. The exact length and seek index cost a scan of the whole file, seconds from a network share,
    // so a second stream scans while this one plays; the length counts from when it is known, and a seek waits until
    // the scan has passed its target. Where the library knows the exact length, that length stands in for the scan's
    // and the scan waits for the first seek, so a file played through is read once. Otherwise (library scans) the
    // length stays mpg123's estimate, from the file alone. mpg123 sees the file from its first frame of sound: it would
    // count the tags before it as audio when it estimates the length of a stream without a header, and decode a VBRI
    // frame, which holds no sound.
    Mpg123Decoder(const std::filesystem::path& path, uint32_t lossyBits, bool measure)
        : path_(path), start_(mpegSoundStart(path)), stream_(std::make_unique<Stream>(path, start_)),
          library_(measure ? libraryLength(path) : std::nullopt) {
        info_.sampleRate = uint32_t(stream_->rate);
        info_.channels = uint32_t(stream_->channels);
        info_.bitsPerSample = lossyBits;
        // mpg123 reads a Xing/Info header but not VBRI, whose VBR stream it would estimate from the first bitrate; the
        // library's length knows better than either.
        const int64_t length = mpg123_length64(stream_->mh());
        const uint64_t vbri = vbriSamples(path);
        vbri_ = vbri != 0;
        estimate_ = library_ ? library_->frames : vbri ? vbri : length > 0 ? uint64_t(length) : 0;
        deferScan_ = measure && library_ && !library_->estimated;
        if (!measure)
            info_.totalFrames = estimate_;
        else if (!deferScan_)
            startScan();
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
        // The stream ends at the exact length even where its LAME frame claims more; after an estimated seek, where its
        // audio does.
        const uint64_t exact = exactLength();
        if (exact && !estimated_) frames = position_ < exact ? size_t(std::min<uint64_t>(frames, exact - position_)) : 0;
        size_t bytes = 0;
        while (!bytes && !ended_ && frames) {
            const int result = mpg123_read(stream_->mh(), dst, frames * info_.channels * sizeof(float), &bytes);
            // A last frame that the end of the file, or an ID3v1 tag, cuts short ends the stream.
            const bool cutShort = result == MPG123_ERR && mpg123_errcode(stream_->mh()) == MPG123_ERR_READER && stream_->atEnd();
            if ((result == MPG123_DONE || cutShort) && !bytes) {
                // The unscanned stream may stop where its LAME frame says, short of the audio; the scan knows.
                if (!estimated_ && continueScanned()) continue;
                // The scan counted the cut frame: it plays as silence, so the length holds.
                if (cutShort && !estimated_ && position_ < exact) {
                    const size_t silent = size_t(std::min<uint64_t>(frames, exact - position_));
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
        // The scan's index lands a seek exactly without reading the file up to it a second time: with the scanned
        // stream once the scan has ended, or lent while it runs once it covers the target. A seek the scan has not
        // reached goes at once to where the target is estimated to be, which only a CBR stream knows exactly.
        estimated_ = false;
        bool guessed = false;
        // A scan the seek starts covers nothing yet, so the seek does not wait for it to open the file; the start needs
        // no index.
        const bool scanStarting = deferScan_ && !scanStarted_ && frame > 0;
        if (scanStarting) startScan();
        if (scanDone_.load(std::memory_order_acquire) || !scan_.joinable()) {
            useScanned();
        } else if (scanStarting || !lendScanIndex(frame)) {
            bool exact = false;
            guessed = estimateIndex(exact);
            estimated_ = guessed && !exact;
        }
        if (mpg123_seek64(stream_->mh(), int64_t(frame), SEEK_SET) != int64_t(frame)) {
            // An estimate the stream does not bear out, such as one past its end, gives way to the scan.
            if (!guessed) stream_->fail(ErrorCode::SeekFailed, "mpg123_seek64");
            estimated_ = false;
            useScanned();
            if (mpg123_seek64(stream_->mh(), int64_t(frame), SEEK_SET) != int64_t(frame))
                stream_->fail(ErrorCode::SeekFailed, "mpg123_seek64");
        }
        position_ = frame;
        ended_ = false;
    }

private:
    void startScan() {
        scanStarted_ = true;
        scan_ = std::thread([this] { scanMain(path_); });
    }

    // The scan's length once known; before it, the library's where it is exact.
    uint64_t exactLength() const { return info_.totalFrames ? info_.totalFrames : deferScan_ && !scanStarted_ ? estimate_ : 0; }

    // Waits for the scan and plays its stream, which knows every frame.
    void useScanned() {
        finishScan(true);
        if (scanned_) stream_ = std::move(scanned_);
    }

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

    // While the scan runs: gives the playing stream a copy of its index if it covers `frame`.
    bool lendScanIndex(uint64_t frame) {
        const int64_t samplesPerFrame = mpg123_spf(stream_->mh());
        if (samplesPerFrame <= 0) return false;
        // Past the target by a few frames for the encoder delay and the frames decoded before it.
        const uint64_t needed = frame / uint64_t(samplesPerFrame) + 8;
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
        return step > 0 && offsets.size() > 1 && uint64_t(offsets.size() - 1) * uint64_t(step) >= needed &&
               mpg123_set_index64(stream_->mh(), offsets.data(), step, offsets.size()) == MPG123_OK;
    }

    // Gives the playing stream an index of where its frames are estimated to start: from the frame size where it holds
    // for every frame, from a Xing table, or else in proportion to the length. `exact` for a CBR stream, as its Info
    // frame or its measured length shows; a length the library estimated shows nothing. False when the stream gives
    // nothing to estimate from.
    bool estimateIndex(bool& exact) {
        mpg123_handle* mh = stream_->mh();
        int64_t* own = nullptr;
        int64_t ownStep = 0;
        size_t ownFill = 0;
        mpg123_frameinfo2 frame{};
        const int64_t samplesPerFrame = mpg123_spf(mh);
        if (mpg123_index64(mh, &own, &ownStep, &ownFill) != MPG123_OK || !ownFill || mpg123_info2(mh, &frame) != MPG123_OK ||
            frame.rate <= 0 || frame.bitrate <= 0 || samplesPerFrame <= 0)
            return false;
        const auto first = double(own[0]); // where mpg123 found its first frame
        const double audio = double(stream_->audioEnd()) - first;
        const double slots = frame.layer == 1 ? 48000.0 : frame.layer == 3 && frame.version != MPG123_1_0 ? 72000.0 : 144000.0;
        const double frameBytes = slots * frame.bitrate / double(frame.rate);
        const uint64_t estimated = estimate_ ? (estimate_ + uint64_t(samplesPerFrame) - 1) / uint64_t(samplesPerFrame) : 0;
        const bool measured = library_ && !library_->estimated;
        const bool noVbrHeader = frame.vbr == MPG123_CBR && !vbri_;
        const bool fits = measured && estimated && std::abs(audio / double(estimated) - frameBytes) <= frameBytes * 0.005;
        const bool cbr = noVbrHeader && (fits || (!measured && lossyLengthDeclared(path_)));
        // Without a header or a measured length, the frame size is the best guess there is.
        const bool bySize = cbr || (noVbrHeader && !measured);
        if (!bySize && !toc_) toc_ = xingToc(path_);
        const double frames = bySize ? audio / frameBytes : toc_ ? double(toc_->frames) : double(estimated);
        if (frames < 1 || audio <= 0) return false;
        constexpr int64_t kStep = 16;   // frames read forward from an entry to the target
        constexpr double kMargin = 2.0; // before the estimate, which a CBR frame size rounds by at most a byte
        std::vector<int64_t> offsets(size_t(frames / double(kStep)) + 1);
        for (size_t i = 0; i < offsets.size(); i++) {
            const double k = double(i) * double(kStep);
            double at = first + audio * k / frames;
            if (bySize) {
                at = first + k * frameBytes;
            } else if (toc_) {
                at = double(toc_->at) - double(start_) + toc_->byteShare(k / frames) * toc_->bytes;
            }
            offsets[i] = i ? std::max(int64_t(first), int64_t(at - kMargin)) : int64_t(first);
        }
        exact = cbr;
        return mpg123_set_index64(mh, offsets.data(), kStep, offsets.size()) == MPG123_OK;
    }

    // Takes the scan's exact length, waiting for the scan when `wait` is set. After an estimated seek the position is
    // itself an estimate, which the exact length would not bear out at the end, so the length waits for a seek that
    // lands exactly.
    void finishScan(bool wait = false) {
        if (scan_.joinable() && (wait || scanDone_.load(std::memory_order_acquire))) {
            scan_.join();
            if (scanned_) scannedLength_ = uint64_t(std::max<int64_t>(0, mpg123_length64(scanned_->mh())));
        }
        if (!estimated_ && scannedLength_ && scannedLength_ >= position_) info_.totalFrames = scannedLength_;
    }

    // At the end of the unscanned stream: continues from the scanned one at the same frame, if it holds more. A stream
    // that reached the library's exact length needs no scan.
    bool continueScanned() {
        if (deferScan_ && !scanStarted_) {
            if (position_ >= estimate_) return false;
            startScan();
        }
        finishScan(true);
        if (!scanned_ || position_ >= info_.totalFrames ||
            mpg123_seek64(scanned_->mh(), int64_t(position_), SEEK_SET) != int64_t(position_))
            return false;
        stream_ = std::move(scanned_);
        return true;
    }

    std::filesystem::path path_;
    uint64_t start_;
    std::unique_ptr<Stream> stream_;
    std::optional<LibraryLength> library_;
    std::optional<XingToc> toc_; // read at the first seek that needs it
    bool vbri_ = false;
    bool deferScan_ = false; // the library knows the exact length, so the scan waits for a seek
    bool scanStarted_ = false;
    bool estimated_ = false;          // positions since the last seek are estimates
    std::unique_ptr<Stream> scanned_; // written by the scan thread before scanDone_; read after joining it
    ScanGate gate_;
    Stream* scanning_ = nullptr; // the scan's stream while it scans, guarded by gate_.mutex
    std::atomic<bool> scanDone_{false};
    std::thread scan_;
    uint64_t scannedLength_ = 0; // the scan's exact length, once it has ended
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

void rememberLibraryLength(const std::filesystem::path& path, uint64_t frames, bool estimated) {
    constexpr size_t kKept = 8;
    const std::wstring key = libraryLengthKey(path);
    const std::scoped_lock lock(libraryLengthsMutex);
    std::erase_if(libraryLengths, [&](const auto& e) { return _wcsicmp(e.first.c_str(), key.c_str()) == 0; });
    libraryLengths.emplace_back(key, LibraryLength{frames, estimated});
    if (libraryLengths.size() > kKept) libraryLengths.erase(libraryLengths.begin());
}

SourceInfo probeMp3Info(const std::filesystem::path& path, bool& estimated) {
    estimated = !lossyLengthDeclared(path);
    const Mpg123Decoder decoder(path, 24, false);
    return decoder.info();
}

} // namespace camel::audio
