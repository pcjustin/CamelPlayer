#include "Engine/PlaybackEngine.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "Engine/decode/FormatProbe.h"
#include "Engine/FormatRule.h"
#include "Engine/RustSession.h"
#include "OutputBridge/OutputBridge.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::audio {

namespace {
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

DsdOutput dsdOutput(const Settings& s) {
    return s.backend == Backend::Asio ? s.asio.dsdOutput : s.backend == Backend::Ks ? s.ks.dsdOutput : s.wasapi.dsdOutput;
}

uint32_t dsdPcmRate(const Settings& s) {
    return s.backend == Backend::Asio ? s.asio.dsdPcmRate : s.backend == Backend::Ks ? s.ks.dsdPcmRate : s.wasapi.dsdPcmRate;
}

// How `src` reaches a device that takes what `supports` accepts under settings `s` (docs/Plan.md §5.3). DSD: the
// configured raw path (ASIO native, else DoP; KS native DSD is not enabled yet and WASAPI has none), PCM only when
// chosen or allowed as a fallback.
std::optional<OutputPlan> planFor(const Settings& s, const SourceInfo& src, bool lossy,
                                  const std::function<bool(const DeviceFormat&)>& supports) {
    if (src.bitsPerSample != 1) return negotiateOutput(src, lossy, fixedSampleType(s), s.convertWhenUnsupported, supports);
    const DsdOutput mode = dsdOutput(s);
    std::optional<OutputPlan> plan;
    if (mode != DsdOutput::Pcm)
        plan = negotiateDsd(src, s.backend == Backend::Asio && mode == DsdOutput::Native, fixedSampleType(s), supports);
    if (!plan && (mode == DsdOutput::Pcm || s.convertDsdWhenUnsupported))
        plan = negotiateDsdToPcm(src, dsdPcmRate(s), fixedSampleType(s), s.convertWhenUnsupported, supports);
    return plan;
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

// The file's own decoder, else the per-format setting; an unsupported pairing fails instead of substituting.
DecoderKind chooseDecoder(AudioFormat format, std::optional<DecoderKind> requested, const Settings& s) {
    if (decodersFor(format).empty())
        throw Error(ErrorCode::UnsupportedStream, std::string(formatName(format)) + " is not supported yet");
    const DecoderKind decoder = requested.value_or(decoderFor(s.decoderByFormat, format));
    if (!supports(format, decoder))
        throw Error(ErrorCode::UnsupportedStream, std::string(decoderName(decoder)) + " cannot decode " + formatName(format));
    return decoder;
}
} // namespace

class PlaybackEngine::Impl final : public IRenderSource {
public:
    Impl(const Settings& s, OutputFactory f) : settings_(s), factory_(std::move(f)) {
        snap_.backend = s.backend;
        snap_.decoder = displayedDecoder(s);
        thread_ = std::thread([this] { controlMain(); });
    }

    ~Impl() override {
        try {
            post([this] {
                teardown();
                output_.reset();
            });
        } catch (...) {
            OutputDebugStringW(L"PlaybackEngine: failed to post teardown during destruction.\n");
        }
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
        if (!rust_) return 0;
        const auto rendered = rust_->render(dst, frames, bpf_);
        const size_t n = rendered.frames;
        const bool drained = rendered.ended || rendered.fault;
        const auto transitions = rust_->transitions();
        if (transitions.count != renderTransitions_.load(std::memory_order_relaxed)) {
            // A queued file became audible inside this request; positions now count from its start.
            renderTransitions_.store(transitions.count, std::memory_order_release);
            base_.store(0, std::memory_order_relaxed);
            consumed_.store(transitions.trackPosition, std::memory_order_relaxed);
            transitionPending_.store(true, std::memory_order_release);
            qCv_.notify_all();
        } else {
            consumed_.fetch_add(n, std::memory_order_relaxed);
        }
        if (n < frames) {
            if (drained) {
                if (!ended_.exchange(true)) qCv_.notify_all();
            } else if (playing_.load(std::memory_order_relaxed)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return n;
    }

    bool writesSilence() const noexcept override { return rawDsd_.load(std::memory_order_acquire); }

    // ---- commands (control thread) ----
    void openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
        file_ = file.wstring();
        fileDecoder_ = decoder;
        startSession(0, true);
    }

    void load(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
        teardown();
        file_ = file.wstring();
        fileDecoder_ = decoder;
        format_.reset();
        position_ = 0;
        std::optional<SourceInfo> source;
        try {
            format_ = probeFormat(file);
            source = openDecoder(chooseDecoder(*format_, decoder, settings_), file, settings_.pcmBitsByFormat)->info();
        } catch (const Error& e) {
            log::info("engine: %s is reported when playback starts (%s)", toUtf8(file_).c_str(), e.what());
        }
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source = source;
            snap_.total = source ? source->totalFrames : 0;
            snap_.format = format_;
            snap_.decoder = displayedDecoder(settings_);
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        setState(PlayState::Stopped);
    }

    void play() {
        if (active_) {
            if (state_ == PlayState::Paused) {
                rust_->play();
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
            rust_->pause();
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
        // A lossy file decodes to the configured depth, so changing it reopens the stream like a decoder change.
        const bool lossyBitsChanged = format_ && isLossy(*format_) &&
                                      pcmBitsFor(settings_.pcmBitsByFormat, *format_) != pcmBitsFor(s.pcmBitsByFormat, *format_);
        // A conversion setting reopens the stream only when the path it would choose differs.
        const bool conversionChanged =
            (settings_.convertWhenUnsupported != s.convertWhenUnsupported || settings_.resampleQuality != s.resampleQuality ||
             settings_.convertDsdWhenUnsupported != s.convertDsdWhenUnsupported) &&
            active_ && !outputChanged && conversionPathChanges(s);
        const bool decoderChanged = displayedDecoder(settings_) != displayedDecoder(s) || lossyBitsChanged || conversionChanged;
        const bool wasActive = active_;
        const bool wasPlaying = state_ == PlayState::Playing;
        const uint64_t pos = currentPosition();
        if ((outputChanged || decoderChanged) && wasActive) teardown();
        if (outputChanged) output_.reset();
        settings_ = s;
        {
            std::lock_guard lock(snapMutex_);
            snap_.backend = s.backend;
            snap_.decoder = displayedDecoder(s);
        }
        if ((outputChanged || decoderChanged) && wasActive)
            startSession(pos, wasPlaying);
        else
            publish();
    }

    bool queueNext(const std::filesystem::path& file, std::optional<DecoderKind> requested) {
        if (!active_ || rust_->transitions().nextQueued) return false;
        AudioFormat format{};
        DecoderKind decoder{};
        SourceInfo source;
        bool lossy = false;
        try {
            format = probeFormat(file);
            decoder = chooseDecoder(format, requested, settings_);
            const auto opened = openDecoder(decoder, file, settings_.pcmBitsByFormat);
            source = opened->info();
            lossy = opened->readsFloat();
        } catch (const Error&) {
            return false; // reported when the file is opened normally
        }
        std::optional<OutputPlan> plan;
        try {
            plan = negotiate(source, lossy);
        } catch (const Error&) {
            return false; // the device question fails again when the file is opened normally
        }
        if (!plan || plan->format != fmt_ || !rust_->queueNext(file, decoder)) return false;
        queued_.push_back({file.wstring(), source, format, requested, decoder, *plan});
        publishQueue();
        return true;
    }

    bool clearNext() {
        if (!rust_ || !rust_->transitions().nextQueued) return true;
        if (!rust_->clearNext()) return false;
        queued_.pop_back();
        publishQueue();
        return true;
    }

    void publishQueue() {
        {
            std::lock_guard lock(snapMutex_);
            snap_.queuedFiles = queued_.size();
            snap_.nextSlotFree = rust_ && !rust_->transitions().nextQueued;
        }
        publish();
    }

    bool showControlPanel() {
        if (settings_.backend != Backend::Asio) return false;
        try {
            ensureOutput();
            return output_->showControlPanel();
        } catch (const Error& e) {
            fail(e, PlayState::Stopped);
            return false;
        }
    }

    EngineSnapshot snapshot() const {
        std::lock_guard lock(snapMutex_);
        EngineSnapshot s = snap_;
        if (active_) {
            s.position = sourcePosition();
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
                qCv_.wait_for(lock, std::chrono::milliseconds(50),
                              [&] { return quit_ || !queue_.empty() || ended_.load() || transitionPending_.load(); });
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
                } catch (const Error& e) {
                    teardown();
                    output_.reset();
                    fail(e, e.code() == ErrorCode::DeviceLost || e.code() == ErrorCode::DeviceInUse ? PlayState::Paused
                                                                                                    : PlayState::Stopped);
                } catch (const std::exception& e) {
                    teardown();
                    output_.reset();
                    fail(Error(ErrorCode::Other, e.what()), PlayState::Stopped);
                }
            }
            if (active_ && transitionPending_.exchange(false)) onTransition();
            if (active_ && ended_.load()) onEnded();
            if (active_) {
                reportAudible();
                refreshNextSlot();
            }
        }
        CoUninitialize();
    }

    // The decoder empties the slot when it moves on to a queued file, seconds before that file is heard; the Server
    // polls this and queues the one after it.
    void refreshNextSlot() {
        const bool free = !rust_->transitions().nextQueued;
        std::lock_guard lock(snapMutex_);
        snap_.nextSlotFree = free;
    }

    // Frames handed to the device minus what its buffer still holds; refined once drivers report latency.
    void reportAudible() {
        uint64_t buffered = 0;
        const OutputStatus status = output_->status(); // drivers add measurements once the stream runs
        {
            std::lock_guard lock(snapMutex_);
            snap_.outputStatus = status;
            buffered = snap_.outputStatus.bufferFrames;
        }
        const uint64_t consumed = consumed_.load(std::memory_order_relaxed);
        rust_->reportAudible(toOutput(base_) + consumed - std::min(buffered, consumed));
    }

    uint64_t currentPosition() const { return active_ ? sourcePosition() : position_; }

    // The device consumes output frames; positions count source frames, which differ when the stream is resampled.
    uint64_t sourcePosition() const {
        const uint64_t consumed = consumed_.load(std::memory_order_relaxed);
        const uint32_t in = sourceRate_.load(std::memory_order_relaxed), out = outputRate_.load(std::memory_order_relaxed);
        return base_ + (in == out || !out ? consumed : (consumed * in + out / 2) / out);
    }
    uint64_t toOutput(uint64_t sourceFrames) const {
        const uint32_t in = sourceRate_.load(std::memory_order_relaxed), out = outputRate_.load(std::memory_order_relaxed);
        return in == out || !in ? sourceFrames : (sourceFrames * out + in / 2) / in;
    }

    std::optional<OutputPlan> negotiate(const SourceInfo& src, bool lossy) {
        return planFor(settings_, src, lossy, [&](const DeviceFormat& f) { return output_->supports(f); });
    }

    RustSession::Conversion conversion() const {
        return {settings_.convertWhenUnsupported, settings_.resampleQuality, plan_.dsd == DsdPath::Pcm};
    }

    void ensureOutput() {
        if (output_) return;
        output_ = std::make_unique<OutputBridge>(factory_(settings_));
    }

    void startSession(uint64_t startFrame, bool playNow) {
        teardown();
        const uint64_t session = ++session_;
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source.reset();
            snap_.total = 0;
            snap_.outputStatus = {};
        }
        std::unique_ptr<IDecoder> dec;
        DecoderKind decoder{};
        try {
            format_.reset();
            format_ = probeFormat(file_);
            decoder = chooseDecoder(*format_, fileDecoder_, settings_);
            dec = openDecoder(decoder, file_, settings_.pcmBitsByFormat);
        } catch (const Error& e) {
            {
                std::lock_guard lock(snapMutex_);
                snap_.format = format_;
            }
            position_ = 0;
            return fail(e, PlayState::Stopped);
        }
        const SourceInfo src = dec->info();
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.format = format_;
            snap_.decoder = decoder;
            snap_.source = src;
            snap_.total = src.totalFrames;
            snap_.output.reset();
            snap_.plan.reset();
        }
        const bool lossy = dec->readsFloat();
        std::optional<OutputPlan> plan;
        try {
            ensureOutput();
            plan = negotiate(src, lossy);
        } catch (const Error& e) {
            position_ = startFrame;
            return fail(e, PlayState::Stopped);
        }
        if (!plan) {
            char msg[200];
            const char* hint = settings_.convertWhenUnsupported ? "" : "; enable \"convert when unsupported\" to convert it";
            const int length =
                src.bitsPerSample == 1
                    ? snprintf(msg, sizeof msg, "DSD%u / %u ch: this device takes neither native DSD nor DoP at %.1f kHz%s",
                               src.sampleRate * 8 / 44100, src.channels, dopCarrierRate(src.sampleRate) / 1000.0,
                               settings_.convertDsdWhenUnsupported ? ""
                                                                   : "; enable \"convert DSD when unsupported\" to convert it")
                    : snprintf(msg, sizeof msg, "%u Hz / %u-bit / %u ch has no bit-perfect format on this device%s",
                               src.sampleRate, src.bitsPerSample, src.channels, hint);
            if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "No bit-perfect format available on this device");
            position_ = 0;
            return fail(Error(ErrorCode::FormatNotSupported, msg), PlayState::Stopped);
        }
        if (startFrame && src.totalFrames && startFrame >= src.totalFrames) startFrame = 0;
        plan_ = *plan;
        fmt_ = plan_.format;
        bpf_ = fmt_.bytesPerFrame();
        sourceRate_ = src.sampleRate;
        outputRate_ = fmt_.sampleRate;
        dec.reset(); // the Rust Engine opens its own decoder through the host ABI
        try {
            rust_ = std::make_unique<RustSession>(file_, decoder, fmt_, startFrame, settings_.pcmBitsByFormat, conversion(),
                                                  plan_.dsd);
        } catch (const Error& e) {
            position_ = 0;
            return fail(e, PlayState::Stopped);
        }

        base_ = startFrame;
        consumed_ = 0;
        prefilled_ = 0;
        underruns_ = 0;
        decodeDone_ = false;
        ended_ = false;
        stopDecode_ = false;
        renderTransitions_ = 0;
        handledTransitions_ = 0;
        transitionPending_ = false;
        {
            std::lock_guard lock(decodeErrMutex_);
            decodeError_.reset();
        }
        decodeThread_ = std::thread([this] { decodeMain(); });

        // Let the decoder get ahead before the device starts pulling.
        const auto startFrames = uint64_t(fmt_.sampleRate * kStartFillSeconds);
        for (int i = 0; i < 600 && bufferedFrames() < startFrames && !decodeDone_; i++) Sleep(5);

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
        rawDsd_.store(plan_.dsd == DsdPath::Native || plan_.dsd == DsdPath::Dop, std::memory_order_release);
        try {
            output_->open(fmt_, this);
        } catch (const Error& e) {
            rawDsd_ = false;
            stopDecoder();
            rust_.reset();
            position_ = startFrame;
            // Device busy or unplugged: pause and wait for the user (docs/Plan.md §2)
            const bool waitForDevice = e.code() == ErrorCode::DeviceInUse || e.code() == ErrorCode::DeviceLost;
            if (!waitForDevice) output_.reset();
            return fail(e, waitForDevice ? PlayState::Paused : PlayState::Stopped);
        }
        active_ = true;
        {
            std::lock_guard lock(snapMutex_);
            snap_.output = fmt_;
            snap_.plan = plan_;
            snap_.outputStatus = output_->status();
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        log::info("engine: %s via %s, %s, start %llu", toUtf8(file_).c_str(), decoderName(decoder), describe(fmt_).c_str(),
                  (unsigned long long)startFrame);
        if (playNow) {
            playing_ = true;
            rust_->play();
            output_->start();
            setState(PlayState::Playing);
        } else {
            setState(PlayState::Paused);
        }
    }

    void decodeMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        try {
            while (!stopDecode_) {
                uint64_t frames = 0;
                const auto result = rust_->prefill(frames);
                if (result == RustSession::Prefill::End) {
                    // Keep polling: a file queued while the tail still plays continues the stream gaplessly.
                    decodeDone_.store(true, std::memory_order_release);
                    Sleep(5);
                    continue;
                }
                if (result == RustSession::Prefill::Full) {
                    Sleep(5);
                    continue;
                }
                prefilled_.fetch_add(frames, std::memory_order_relaxed);
            }
        } catch (const Error& e) {
            log::error("engine: decode error: %s", e.what());
            std::lock_guard lock(decodeErrMutex_);
            decodeError_ = e;
        }
        decodeDone_.store(true, std::memory_order_release);
    }

    uint64_t bufferedFrames() const { return prefilled_.load(std::memory_order_relaxed); }

    void stopDecoder() {
        stopDecode_ = true;
        if (decodeThread_.joinable()) decodeThread_.join();
    }

    void teardown() {
        if (output_) {
            try {
                output_->close();
            } catch (const std::exception& e) {
                log::warn("engine: closing output failed: %s", e.what());
                output_.reset();
            }
        }
        rawDsd_ = false; // the output no longer renders
        stopDecoder();
        rust_.reset();
        if (active_) position_ = sourcePosition();
        active_ = false;
        playing_ = false;
        ended_ = false;
        transitionPending_ = false;
        queued_.clear();
        std::lock_guard lock(snapMutex_);
        snap_.output.reset();
        snap_.plan.reset();
        snap_.queuedFiles = 0;
        snap_.nextSlotFree = false;
    }

    // Queued files are now audible: the last of them becomes the current file without reopening the device.
    void onTransition() {
        const uint64_t audible = renderTransitions_.load(std::memory_order_acquire);
        if (audible == handledTransitions_) return;
        while (handledTransitions_ < audible && !queued_.empty()) {
            ++handledTransitions_;
            const Queued next = queued_.front();
            queued_.pop_front();
            file_ = next.file;
            plan_ = next.plan;
            sourceRate_ = next.source.sampleRate;
            fileDecoder_ = next.requested;
            format_ = next.format;
            const auto& source = next.source;
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.format = next.format;
            snap_.decoder = next.decoder;
            snap_.plan = next.plan;
            snap_.source = source;
            snap_.total = source.totalFrames;
            snap_.transitions++;
        }
        handledTransitions_ = audible;
        publishQueue();
    }

    void onEnded() {
        stopDecoder(); // the worker may still be recording the failure that ended the stream
        std::optional<Error> err;
        {
            std::lock_guard lock(decodeErrMutex_);
            err = decodeError_;
        }
        teardown();
        position_ = 0;
        if (err) return fail(*err, PlayState::Stopped);
        {
            std::lock_guard lock(snapMutex_);
            snap_.trackEnds++;
        }
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
            if (s == PlayState::Stopped) snap_.trackEnds++;
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
        if (l) {
            try {
                l();
            } catch (...) {
                log::error("engine: state listener failed");
            }
        }
    }

    // Whether settings `s` would convert the open stream differently: another path, or another resampling quality.
    bool conversionPathChanges(const Settings& s) {
        if (plan_.resample && settings_.resampleQuality != s.resampleQuality) return true;
        const auto source = snapshot().source;
        if (!source) return false;
        const auto plan =
            planFor(s, *source, format_ && isLossy(*format_), [&](const DeviceFormat& f) { return output_->supports(f); });
        return plan != plan_;
    }

    // The decoder the current file uses, or would use, under settings `s`.
    DecoderKind displayedDecoder(const Settings& s) const {
        if (fileDecoder_) return *fileDecoder_;
        const auto format = format_ && !decodersFor(*format_).empty() ? *format_ : AudioFormat::Flac;
        return decoderFor(s.decoderByFormat, format);
    }

    // control-thread state
    Settings settings_;
    OutputFactory factory_;
    std::unique_ptr<OutputBridge> output_;
    std::unique_ptr<RustSession> rust_;
    std::wstring file_;
    std::optional<DecoderKind> fileDecoder_; // per-file override of the per-format setting
    std::optional<AudioFormat> format_;
    PlayState state_ = PlayState::Stopped;
    uint64_t position_ = 0;
    uint64_t session_ = 0;
    std::atomic<bool> active_{false};
    DeviceFormat fmt_;
    OutputPlan plan_;
    std::atomic<uint32_t> sourceRate_{0}, outputRate_{0}; // read by snapshot() while the stream plays

    // shared with the decode / output threads
    size_t bpf_ = 8;
    std::atomic<uint64_t> base_{0}, consumed_{0}, prefilled_{0}, underruns_{0};
    std::atomic<bool> decodeDone_{false}, ended_{false}, stopDecode_{false}, playing_{false}, transitionPending_{false};
    std::atomic<bool> rawDsd_{false};            // the open stream is DoP or native DSD
    std::atomic<uint64_t> renderTransitions_{0}; // written by the output callback
    uint64_t handledTransitions_ = 0;
    struct Queued {
        std::wstring file;
        SourceInfo source;
        AudioFormat format;
        std::optional<DecoderKind> requested;
        DecoderKind decoder;
        OutputPlan plan;
    };
    std::deque<Queued> queued_; // queued files not yet audible: at most one in the decoder and one in the slot
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

void PlaybackEngine::openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
    auto fileCopy = std::make_shared<std::filesystem::path>(file);
    impl_->post([this, fileCopy, decoder] { impl_->openAndPlay(*fileCopy, decoder); });
}
void PlaybackEngine::load(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
    auto fileCopy = std::make_shared<std::filesystem::path>(file);
    impl_->post([this, fileCopy, decoder] { impl_->load(*fileCopy, decoder); });
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
    auto settingsCopy = std::make_shared<Settings>(s);
    impl_->post([this, settingsCopy] { impl_->applySettings(*settingsCopy); });
}
bool PlaybackEngine::showControlPanel() {
    return impl_->call<bool>([this] { return impl_->showControlPanel(); });
}
bool PlaybackEngine::queueNext(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
    return impl_->call<bool>([this, &file, decoder] { return impl_->queueNext(file, decoder); });
}
bool PlaybackEngine::clearNext() {
    return impl_->call<bool>([this] { return impl_->clearNext(); });
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
