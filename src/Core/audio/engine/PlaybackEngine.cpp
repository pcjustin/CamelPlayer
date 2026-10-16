#include "audio/engine/PlaybackEngine.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/decode/FlacStreamInfo.h"
#include "audio/engine/FormatRule.h"
#include "audio/engine/SamplePacker.h"
#include "audio/engine/SpscRing.h"
#include "audio/output/AsioOutput.h"
#include "util/Log.h"
#include "util/Text.h"

namespace camel::audio {

namespace {
constexpr size_t kDecodeChunk = 4096;     // frames per decoder read
constexpr double kStartFillSeconds = 0.5; // buffered before the output starts

std::optional<SampleType> fixedSampleType(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return s.wasapi.sampleType;
    case Backend::Ks:
        return s.ks.sampleType;
    case Backend::Asio:
        return std::nullopt; // the driver dictates the type
    }
    return std::nullopt;
}

bool sameBackendConfig(const Settings& a, const Settings& b) {
    if (a.backend != b.backend) return false;
    switch (a.backend) {
    case Backend::Wasapi:
        return a.wasapi == b.wasapi;
    case Backend::Asio:
        return a.asio == b.asio;
    case Backend::Ks:
        return a.ks == b.ks;
    }
    return false;
}
} // namespace

class PlaybackEngine::Impl final : public IRenderSource {
public:
    Impl(const Settings& s, OutputFactory f) : settings_(s), factory_(std::move(f)) {
        snap_.backend = s.backend;
        snap_.decoder = s.decoder;
        thread_ = std::thread([this] { controlMain(); });
    }

    ~Impl() override {
        post([this] {
            teardown();
            output_.reset();
        });
        {
            std::lock_guard lock(qMutex_);
            quit_ = true;
        }
        qCv_.notify_all();
        thread_.join();
    }

    // ---- command posting ----
    void post(std::function<void()> fn) {
        {
            std::lock_guard lock(qMutex_);
            queue_.push_back(std::move(fn));
        }
        qCv_.notify_all();
    }

    template <class R> R call(std::function<R()> fn) {
        std::packaged_task<R()> task(std::move(fn));
        auto fut = task.get_future();
        post([&task] { task(); });
        return fut.get();
    }

    // ---- IRenderSource (real-time thread) ----
    size_t render(uint8_t* dst, size_t frames) noexcept override {
        const size_t bpf = bpf_;
        size_t n = ring_.read(dst, frames * bpf) / bpf;
        consumed_.fetch_add(n, std::memory_order_relaxed);
        if (n < frames) {
            if (decodeDone_.load(std::memory_order_acquire)) {
                if (!ended_.exchange(true)) qCv_.notify_all();
            } else if (playing_.load(std::memory_order_relaxed)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return n;
    }

    // ---- commands (control thread) ----
    void openAndPlay(const std::filesystem::path& file) {
        file_ = file.wstring();
        startSession(0, true);
    }

    void load(const std::filesystem::path& file) {
        teardown();
        file_ = file.wstring();
        position_ = 0;
        SourceInfo si;
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source.reset();
            snap_.total = 0;
            if (readFlacStreamInfo(file, si)) {
                snap_.source = si;
                snap_.total = si.totalFrames;
            }
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        setState(PlayState::Stopped);
    }

    void play() {
        if (active_) {
            if (state_ == PlayState::Paused) {
                output_->start();
                setState(PlayState::Playing);
            }
            return;
        }
        if (!file_.empty()) startSession(position_, true);
    }

    void pause() {
        if (active_ && state_ == PlayState::Playing) {
            output_->pause();
            setState(PlayState::Paused);
        }
    }

    void stop() {
        teardown();
        position_ = 0;
        setState(PlayState::Stopped);
    }

    void seek(uint64_t frame) {
        if (active_)
            startSession(frame, state_ == PlayState::Playing);
        else {
            position_ = frame;
            publish();
        }
    }

    void applySettings(const Settings& s) {
        const bool outputChanged = !sameBackendConfig(settings_, s);
        const bool decoderChanged = settings_.decoder != s.decoder;
        const bool wasActive = active_;
        const bool wasPlaying = state_ == PlayState::Playing;
        const uint64_t pos = currentPosition();
        if ((outputChanged || decoderChanged) && wasActive) teardown();
        if (outputChanged) output_.reset();
        settings_ = s;
        {
            std::lock_guard lock(snapMutex_);
            snap_.backend = s.backend;
            snap_.decoder = s.decoder;
        }
        if ((outputChanged || decoderChanged) && wasActive)
            startSession(pos, wasPlaying);
        else
            publish();
    }

    bool showControlPanel() {
        if (settings_.backend != Backend::Asio) return false;
        try {
            ensureOutput();
            return showAsioControlPanel(output_.get());
        } catch (const Error& e) {
            fail(e, PlayState::Stopped);
            return false;
        }
    }

    EngineSnapshot snapshot() const {
        std::lock_guard lock(snapMutex_);
        EngineSnapshot s = snap_;
        if (active_) {
            s.position = base_ + consumed_.load(std::memory_order_relaxed);
            s.underruns = underruns_.load(std::memory_order_relaxed);
        }
        return s;
    }

    void setListener(std::function<void()> l) {
        std::lock_guard lock(snapMutex_);
        listener_ = std::move(l);
    }

    bool waitForState(PlayState s, std::chrono::milliseconds timeout) {
        std::unique_lock lock(snapMutex_);
        return snapCv_.wait_for(lock, timeout, [&] { return snap_.state == s; });
    }

private:
    void controlMain() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock lock(qMutex_);
                qCv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return quit_ || !queue_.empty() || ended_.load(); });
                if (!queue_.empty()) {
                    fn = std::move(queue_.front());
                    queue_.pop_front();
                } else if (quit_) {
                    break;
                }
            }
            if (fn) {
                try {
                    fn();
                } catch (const std::exception& e) {
                    log::error("engine: command failed: %s", e.what());
                }
            }
            if (active_ && ended_.load()) onEnded();
        }
        CoUninitialize();
    }

    uint64_t currentPosition() const { return active_ ? base_ + consumed_.load(std::memory_order_relaxed) : position_; }

    void ensureOutput() {
        if (output_) return;
        output_ = factory_(settings_);
    }

    void startSession(uint64_t startFrame, bool playNow) {
        teardown();
        const uint64_t session = ++session_;
        std::unique_ptr<IDecoder> dec;
        try {
            dec = openDecoder(settings_.decoder, file_);
        } catch (const Error& e) {
            position_ = 0;
            return fail(e, PlayState::Stopped);
        }
        const SourceInfo src = dec->info();
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source = src;
            snap_.total = src.totalFrames;
            snap_.output.reset();
        }
        std::optional<DeviceFormat> fmt;
        try {
            ensureOutput();
            fmt =
                chooseOutputFormat(src, fixedSampleType(settings_), [&](const DeviceFormat& f) { return output_->supports(f); });
        } catch (const Error& e) {
            position_ = startFrame;
            return fail(e, PlayState::Stopped);
        }
        if (!fmt) {
            char msg[128];
            snprintf(msg, sizeof msg, "%u Hz / %u-bit / %u ch has no bit-perfect format on this device", src.sampleRate,
                     src.bitsPerSample, src.channels);
            position_ = 0;
            return fail(Error(ErrorCode::FormatNotSupported, msg), PlayState::Stopped);
        }
        if (startFrame && src.totalFrames && startFrame >= src.totalFrames) startFrame = 0;
        try {
            if (startFrame) dec->seek(startFrame);
        } catch (const Error& e) {
            position_ = 0;
            return fail(e, PlayState::Stopped);
        }

        decoder_ = std::move(dec);
        fmt_ = *fmt;
        bpf_ = fmt_.bytesPerFrame();
        const size_t ringFrames = size_t(fmt_.sampleRate * kPrebufferSeconds) + kDecodeChunk;
        ring_.reset(ringFrames * bpf_);
        base_ = startFrame;
        consumed_ = 0;
        underruns_ = 0;
        decodeDone_ = false;
        ended_ = false;
        stopDecode_ = false;
        {
            std::lock_guard lock(decodeErrMutex_);
            decodeError_.reset();
        }
        decodeThread_ = std::thread([this] { decodeMain(); });

        // Let the decoder get ahead before the device starts pulling.
        const size_t startBytes = size_t(fmt_.sampleRate * kStartFillSeconds) * bpf_;
        for (int i = 0; i < 600 && ring_.readable() < startBytes && !decodeDone_; i++) Sleep(5);

        output_->setErrorHandler([this, session](const Error& e) {
            post([this, session, e] {
                if (session == session_ && active_) onOutputError(e);
            });
        });
        output_->setRestartHandler([this, session] {
            post([this, session] {
                if (session == session_ && active_) onRestart();
            });
        });
        try {
            output_->open(fmt_, this);
        } catch (const Error& e) {
            stopDecoder();
            decoder_.reset();
            position_ = startFrame;
            // Device busy or unplugged: pause and wait for the user (docs/計劃書.md §2)
            const bool waitForDevice = e.code() == ErrorCode::DeviceInUse || e.code() == ErrorCode::DeviceLost;
            if (!waitForDevice) output_.reset();
            return fail(e, waitForDevice ? PlayState::Paused : PlayState::Stopped);
        }
        active_ = true;
        {
            std::lock_guard lock(snapMutex_);
            snap_.output = fmt_;
            snap_.outputStatus = output_->status();
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        log::info("engine: %s via %s, %s, start %llu", toUtf8(file_).c_str(), decoderName(settings_.decoder),
                  describe(fmt_).c_str(), (unsigned long long)startFrame);
        if (playNow) {
            playing_ = true;
            output_->start();
            setState(PlayState::Playing);
        } else {
            setState(PlayState::Paused);
        }
    }

    void decodeMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const SourceInfo& in = decoder_->info();
        std::vector<int32_t> pcm(kDecodeChunk * in.channels);
        std::vector<uint8_t> packed(kDecodeChunk * bpf_);
        try {
            while (!stopDecode_) {
                if (ring_.writable() < packed.size()) {
                    Sleep(5);
                    continue;
                }
                size_t n = decoder_->read(pcm.data(), kDecodeChunk);
                if (!n) break;
                packSamples(pcm.data(), n * in.channels, in.bitsPerSample, fmt_.type, packed.data());
                ring_.write(packed.data(), n * bpf_);
            }
        } catch (const Error& e) {
            log::error("engine: decode error: %s", e.what());
            std::lock_guard lock(decodeErrMutex_);
            decodeError_ = e;
        }
        decodeDone_.store(true, std::memory_order_release);
    }

    void stopDecoder() {
        stopDecode_ = true;
        if (decodeThread_.joinable()) decodeThread_.join();
    }

    void teardown() {
        if (output_) output_->close();
        stopDecoder();
        decoder_.reset();
        if (active_) position_ = base_ + consumed_.load();
        active_ = false;
        playing_ = false;
        ended_ = false;
        std::lock_guard lock(snapMutex_);
        snap_.output.reset();
    }

    void onEnded() {
        std::optional<Error> err;
        {
            std::lock_guard lock(decodeErrMutex_);
            err = decodeError_;
        }
        teardown();
        position_ = 0;
        if (err) return fail(*err, PlayState::Stopped);
        setState(PlayState::Stopped);
    }

    void onOutputError(const Error& e) {
        log::warn("engine: output error %s: %s", errorCodeName(e.code()), e.what());
        teardown();
        output_.reset(); // the device may have changed; reopen from scratch on the next play
        fail(e, PlayState::Paused);
    }

    void onRestart() {
        const bool wasPlaying = state_ == PlayState::Playing;
        const uint64_t pos = currentPosition();
        teardown();
        output_.reset();
        startSession(pos, wasPlaying);
    }

    void fail(const Error& e, PlayState s) {
        log::warn("engine: %s: %s", errorCodeName(e.code()), e.what());
        {
            std::lock_guard lock(snapMutex_);
            snap_.error = e.code();
            snap_.errorDetail = e.what();
        }
        setState(s);
    }

    void setState(PlayState s) {
        state_ = s;
        playing_ = s == PlayState::Playing;
        publish();
    }

    void publish() {
        std::function<void()> l;
        {
            std::lock_guard lock(snapMutex_);
            snap_.state = state_;
            snap_.position = currentPosition();
            snap_.revision++;
            if (!active_) snap_.underruns = 0;
            l = listener_;
        }
        snapCv_.notify_all();
        if (l) l();
    }

    // control-thread state
    Settings settings_;
    OutputFactory factory_;
    std::unique_ptr<IOutput> output_;
    std::unique_ptr<IDecoder> decoder_;
    std::wstring file_;
    PlayState state_ = PlayState::Stopped;
    uint64_t position_ = 0;
    uint64_t session_ = 0;
    std::atomic<bool> active_{false};
    DeviceFormat fmt_;

    // shared with the decode / output threads
    SpscRing ring_;
    size_t bpf_ = 8;
    std::atomic<uint64_t> base_{0}, consumed_{0}, underruns_{0};
    std::atomic<bool> decodeDone_{false}, ended_{false}, stopDecode_{false}, playing_{false};
    std::thread decodeThread_;
    std::mutex decodeErrMutex_;
    std::optional<Error> decodeError_;

    std::thread thread_;
    std::mutex qMutex_;
    std::condition_variable qCv_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;

    mutable std::mutex snapMutex_;
    std::condition_variable snapCv_;
    EngineSnapshot snap_;
    std::function<void()> listener_;
};

PlaybackEngine::PlaybackEngine(const Settings& s, OutputFactory f) : impl_(std::make_unique<Impl>(s, std::move(f))) {}
PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::openAndPlay(const std::filesystem::path& file) {
    impl_->post([this, file] { impl_->openAndPlay(file); });
}
void PlaybackEngine::load(const std::filesystem::path& file) {
    impl_->post([this, file] { impl_->load(file); });
}
void PlaybackEngine::play() {
    impl_->post([this] { impl_->play(); });
}
void PlaybackEngine::pause() {
    impl_->post([this] { impl_->pause(); });
}
void PlaybackEngine::togglePause() {
    impl_->post([this] {
        if (impl_->snapshot().state == PlayState::Playing)
            impl_->pause();
        else
            impl_->play();
    });
}
void PlaybackEngine::stop() {
    impl_->post([this] { impl_->stop(); });
}
void PlaybackEngine::seek(uint64_t frame) {
    impl_->post([this, frame] { impl_->seek(frame); });
}
void PlaybackEngine::applySettings(const Settings& s) {
    impl_->post([this, s] { impl_->applySettings(s); });
}
bool PlaybackEngine::showControlPanel() {
    return impl_->call<bool>([this] { return impl_->showControlPanel(); });
}
EngineSnapshot PlaybackEngine::snapshot() const {
    return impl_->snapshot();
}
void PlaybackEngine::setListener(std::function<void()> l) {
    impl_->setListener(std::move(l));
}
void PlaybackEngine::waitIdle() {
    impl_->call<void>([] {});
}
bool PlaybackEngine::waitForState(PlayState s, std::chrono::milliseconds t) {
    return impl_->waitForState(s, t);
}

} // namespace camel::audio
