// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/PlaybackEngine.h"

#include <windows.h>
#include <objbase.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "Engine/decode/FormatProbe.h"
#include "Engine/Equalizer.h"
#include "Engine/FormatRule.h"
#include "Engine/RustSession.h"
#include "OutputBridge/OutputBridge.h"
#include "OutputBridge/OutputChoices.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
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
// configured raw path (ASIO native, else DoP; WASAPI and KS have no native DSD), PCM only when
// chosen or allowed as a fallback. A fixed rate the device refuses falls back to the file's rate, noted in `adjustment`.
std::optional<OutputPlan> planFor(const Settings& s, const SourceInfo& src, bool lossy,
                                  const std::function<bool(const DeviceFormat&)>& supports, std::string* adjustment = nullptr) {
    if (s.backend == Backend::Wasapi && s.wasapi.shared) {
        // Choosing shared mode permits converting to the mixer's float32 format; nothing here is bit-perfect, and raw
        // DSD cannot pass the mixer.
        std::optional<OutputPlan> plan;
        if (src.bitsPerSample != 1)
            plan = negotiateOutput(src, lossy, SampleType::Float32, true, supports);
        else if (dsdOutput(s) == DsdOutput::Pcm || s.convertDsdWhenUnsupported)
            plan = negotiateDsdToPcm(src, dsdPcmRate(s), SampleType::Float32, true, supports);
        if (plan) {
            plan->shared = true;
            plan->state = PipelineState::Processed;
        }
        return plan;
    }
    if (src.bitsPerSample != 1) {
        const uint32_t rate = fixedRate(s);
        if (rate)
            if (auto plan = negotiateOutput(src, lossy, fixedSampleType(s), s.convertWhenUnsupported, supports, rate))
                return plan;
        auto plan = negotiateOutput(src, lossy, fixedSampleType(s), s.convertWhenUnsupported, supports);
        if (rate && plan && adjustment)
            *adjustment = requestedVsActive("Fixed sample rate", std::to_string(rate) + " Hz",
                                            std::to_string(plan->format.sampleRate) + " Hz (the device refused it)");
        return plan;
    }
    const DsdOutput mode = dsdOutput(s);
    std::optional<OutputPlan> plan;
    if (mode != DsdOutput::Pcm)
        plan = negotiateDsd(src, s.backend == Backend::Asio && mode == DsdOutput::Native, fixedSampleType(s), supports);
    if (!plan && (mode == DsdOutput::Pcm || s.convertDsdWhenUnsupported))
        plan = negotiateDsdToPcm(src, dsdPcmRate(s), fixedSampleType(s), s.convertWhenUnsupported, supports);
    return plan;
}

// Timing applies without reopening the stream.
template <class T> bool sameOutput(T a, const T& b) {
    a.timing = b.timing;
    return a == b;
}

bool sameBackendConfig(const Settings& a, const Settings& b) {
    if (a.backend != b.backend) return false;
    switch (a.backend) {
    case Backend::Wasapi:
        return sameOutput(a.wasapi, b.wasapi);
    case Backend::Asio:
        return sameOutput(a.asio, b.asio);
    case Backend::Ks:
        return sameOutput(a.ks, b.ks);
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
    Impl(const Settings& s, OutputFactory f, DeviceWatcherFactory w)
        : settings_(s), factory_(std::move(f)), watcherFactory_(std::move(w)) {
        snap_.backend = s.backend;
        snap_.decoder = displayedDecoder(s);
        thread_ = std::thread([this] { controlMain(); });
        post([this] { updateWatcher(); });
    }

    ~Impl() override {
        try {
            post([this] {
                watcher_.reset();
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
        if (uint64_t lead = leadSilence_.load(std::memory_order_acquire)) {
            // The whole request is silence; the paused Engine writes the DSD idle pattern with continuing DoP markers.
            if (rawDsd_.load(std::memory_order_acquire))
                rust_->render(dst, frames, bpf_);
            else
                memset(dst, 0, frames * bpf_);
            const uint64_t left = lead > frames ? lead - frames : 0;
            if (leadSilence_.compare_exchange_strong(lead, left, std::memory_order_acq_rel) && !left) {
                leadDone_.store(true, std::memory_order_release);
                qCv_.notify_all();
            }
            return frames;
        }
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
        if (n && !firstAudio_.load(std::memory_order_relaxed)) firstAudio_.store(measure::now(), std::memory_order_release);
        if (n < frames) {
            if (drained) {
                if (!ended_.exchange(true)) qCv_.notify_all();
            } else if (playing_.load(std::memory_order_relaxed) && !leadDone_.load(std::memory_order_acquire)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return n;
    }

    bool writesSilence() const noexcept override { return rawDsd_.load(std::memory_order_acquire); }

    // ---- commands (control thread) ----
    void openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
        file_ = file.wstring();
        fileDecoder_ = decoder;
        startSession(0, true);
    }

    void load(const std::filesystem::path& file, std::optional<DecoderKind> decoder) {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
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
            snap_.md5.reset();
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
                startRust();
                output_->start();
                setState(PlayState::Playing);
            }
            return;
        }
        if (!file_.empty()) startSession(position_, true);
    }

    void pause() {
        resumeOnDefault_ = false;
        if (active_ && state_ == PlayState::Playing) {
            output_->pause();
            leadSilence_ = 0;
            leadDone_ = false;
            rust_->pause();
            setState(PlayState::Paused);
        }
    }

    void stop() {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
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

    // `plan` as the EQ leaves it: processed while the EQ changes the signal; raw DSD bypasses it.
    OutputPlan withEq(OutputPlan plan) const {
        plan.eq =
            plan.dsd != DsdPath::Native && plan.dsd != DsdPath::Dop && analyzeEq(settings_.eq, plan.format.sampleRate).active;
        if (plan.eq) plan.state = PipelineState::Processed;
        return plan;
    }

    // The EQ changes without reopening anything: the Engine crossfades to it, and the path shows it.
    void applyEq() {
        if (!active_ || !rust_) return;
        try {
            rust_->setEq(settings_.eq);
        } catch (const Error& e) {
            log::warn("engine: %s", e.what());
        }
        plan_ = withEq(negotiated_);
        std::lock_guard lock(snapMutex_);
        if (snap_.plan) snap_.plan = plan_;
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
        const bool eqChanged = settings_.eq != s.eq;
        const bool wasActive = active_;
        const bool wasPlaying = state_ == PlayState::Playing;
        const uint64_t pos = currentPosition();
        if ((outputChanged || decoderChanged) && wasActive) teardown();
        if (outputChanged) output_.reset();
        settings_ = s;
        updateWatcher();
        {
            std::lock_guard lock(snapMutex_);
            snap_.backend = s.backend;
            snap_.decoder = displayedDecoder(s);
        }
        if ((outputChanged || decoderChanged) && wasActive) {
            startSession(pos, wasPlaying);
        } else {
            if (eqChanged) applyEq();
            publish();
        }
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

    void discardPreloads() {
        if (!active_) return;
        onTransition();
        if (queued_.empty()) return;
        clearNext();
        if (queued_.empty()) return;
        // Close waits for render callbacks before choosing the audible track and restart position.
        closeOutput();
        onTransition();
        const auto position = currentPosition();
        const bool playing = state_ == PlayState::Playing;
        teardown();
        startSession(position, playing);
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
                qCv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return quit_ || !queue_.empty() || ended_.load() || transitionPending_.load() || leadDone_.load();
                });
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
            if (active_ && leadDone_.load()) {
                if (state_ == PlayState::Playing) rust_->play();
                leadDone_ = false;
            }
            if (active_ && transitionPending_.exchange(false)) onTransition();
            if (active_ && ended_.load()) onEnded();
            if (active_) {
                logFirstAudio();
                logUnderruns();
                reportAudible();
                refreshNextSlot();
                refreshMd5();
            }
            if (active_ && state_ == PlayState::Paused) releaseAfterPause();
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

    void refreshMd5() {
        const std::optional<Md5State> md5 = format_ == AudioFormat::Flac ? std::optional(rust_->md5State()) : std::nullopt;
        {
            std::lock_guard lock(snapMutex_);
            if (snap_.md5 == md5) return;
            snap_.md5 = md5;
        }
        publish();
    }

    // Dropouts are otherwise only a counter in the status page, which a new track resets.
    void logUnderruns() {
        const uint64_t count = underruns_.load(std::memory_order_relaxed);
        if (count <= loggedUnderruns_) return;
        log::info("engine: %llu underrun(s) at %.1f s of %s", (unsigned long long)(count - loggedUnderruns_),
                  framesToSeconds(currentPosition(), sourceRate_.load(std::memory_order_relaxed)), toUtf8(file_).c_str());
        loggedUnderruns_ = count;
    }

    // The driver's thread was held up: the ring had audio, yet the device may have played a gap.
    void logLateCallbacks(const OutputStatus& status) {
        if (status.lateCallbacks < loggedLateCallbacks_) loggedLateCallbacks_ = 0; // the output was reopened
        const auto now = std::chrono::steady_clock::now();
        if (status.lateCallbacks == loggedLateCallbacks_ || now - lateCallbacksLoggedAt_ < std::chrono::seconds(1)) return;
        lateCallbacksLoggedAt_ = now;
        log::info("output: %llu late driver callback(s) at %.1f s of %s (longest gap %.1f ms, period %.1f ms)",
                  (unsigned long long)(status.lateCallbacks - loggedLateCallbacks_),
                  framesToSeconds(currentPosition(), sourceRate_.load(std::memory_order_relaxed)), toUtf8(file_).c_str(),
                  status.longestGapMs, status.periodMs);
        loggedLateCallbacks_ = status.lateCallbacks;
    }

    void logFirstAudio() {
        const int64_t at = firstAudio_.load(std::memory_order_acquire);
        if (firstAudioLogged_ || !at) return;
        firstAudioLogged_ = true;
        const int64_t requested = measure::playRequested.exchange(0, std::memory_order_relaxed);
        if (requested && requested <= openedAt_)
            log::info("engine: first audio %.0f ms after the play request (%.0f ms after opening)",
                      measure::milliseconds(requested, at), measure::milliseconds(openedAt_, at));
        else
            log::info("engine: first audio %.0f ms after opening", measure::milliseconds(openedAt_, at));
        if (measure::measuring()) {
            const auto step = [&](int from, int to) { return measure::milliseconds(stages_[from], stages_[to]); };
            log::info("measure: start stages: probe %.0f, decoder %.0f, output plan %.0f, Rust decoder %.0f, buffer %.0f, "
                      "device open %.0f, first pull %.0f ms",
                      measure::milliseconds(openedAt_, stages_[0]), step(0, 1), step(1, 3), step(3, 4), step(4, 5), step(5, 6),
                      measure::milliseconds(stages_[6], at));
        }
    }

    // Frames handed to the device minus what its buffer still holds; refined once drivers report latency.
    void reportAudible() {
        uint64_t buffered = 0;
        const OutputStatus status = outputStatus(); // drivers add measurements once the stream runs
        logLateCallbacks(status);
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

    std::optional<OutputPlan> negotiate(const SourceInfo& src, bool lossy, std::string* adjustment = nullptr) {
        return planFor(settings_, src, lossy, [&](const DeviceFormat& f) { return output_->supports(f); }, adjustment);
    }

    RustSession::Conversion conversion() const {
        if (plan_.shared) return {true, settings_.wasapi.sharedResampleQuality, plan_.dsd == DsdPath::Pcm};
        // A fixed rate is the permission to resample to it (docs/Plan.md §6).
        const bool fixedResample = plan_.dsd == DsdPath::None && plan_.resample && fixedRate(settings_);
        return {settings_.convertWhenUnsupported || fixedResample, settings_.resampleQuality, plan_.dsd == DsdPath::Pcm};
    }

    void ensureOutput() {
        if (output_) return;
        output_ = std::make_unique<OutputBridge>(factory_(settings_));
        ++outputGeneration_;
    }

    OutputStatus outputStatus() const {
        OutputStatus status = output_->status();
        if (!planAdjustment_.empty()) status.adjustments.push_back(planAdjustment_);
        return status;
    }

    // After a format change the device first plays the configured silence (docs/Plan.md §6): the Engine stays paused,
    // rendering silence in the stream's format, until the output has taken that many frames.
    void startRust() {
        const uint32_t ms = timing(settings_).formatChangeSilenceMs;
        if (pendingSilence_ && ms)
            leadSilence_ = std::max<uint64_t>(1, uint64_t(fmt_.sampleRate) * ms / 1000);
        else
            rust_->play();
        pendingSilence_ = false;
    }

    // Closes the device once playback has been paused for the configured time; play reopens it at the same position.
    void releaseAfterPause() {
        const uint32_t seconds = timing(settings_).releaseAfterPauseSeconds;
        if (!seconds || std::chrono::steady_clock::now() - pausedAt_ < std::chrono::seconds(seconds)) return;
        log::info("engine: releasing the output device after %u s paused", seconds);
        teardown();
        output_.reset();
        {
            std::lock_guard lock(snapMutex_);
            snap_.outputStatus = {};
        }
        publish();
    }

    // `deviceChange`: reopening after the Windows default changed; a device that cannot play pauses instead of ending the
    // track, so the user can resume (docs/Plan.md §2).
    void startSession(uint64_t startFrame, bool playNow, bool deviceChange = false) {
        const PlayState onDeviceFailure = deviceChange ? PlayState::Paused : PlayState::Stopped;
        teardown();
        const uint64_t session = ++session_;
        openedAt_ = measure::now();
        firstAudio_ = 0;
        firstAudioLogged_ = false;
        loggedUnderruns_ = 0;
        loggedLateCallbacks_ = 0;
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source.reset();
            snap_.md5.reset();
            snap_.total = 0;
            snap_.outputStatus = {};
        }
        std::unique_ptr<IDecoder> dec;
        DecoderKind decoder{};
        try {
            format_.reset();
            format_ = probeFormat(file_);
            stages_[0] = measure::now();
            decoder = chooseDecoder(*format_, fileDecoder_, settings_);
            dec = openDecoder(decoder, file_, settings_.pcmBitsByFormat);
            stages_[1] = measure::now();
        } catch (const Error& e) {
            {
                std::lock_guard lock(snapMutex_);
                snap_.format = format_;
            }
            position_ = waitingForSource_ ? startFrame : 0;
            return fail(e, waitingForSource_ ? PlayState::Paused : PlayState::Stopped);
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
        planAdjustment_.clear();
        try {
            ensureOutput();
            plan = negotiate(src, lossy, &planAdjustment_);
            stages_[2] = measure::now();
        } catch (const Error& e) {
            position_ = startFrame;
            return fail(e, onDeviceFailure);
        }
        if (!plan) {
            char msg[200];
            const char* hint = settings_.convertWhenUnsupported ? "" : "; enable \"convert when unsupported\" to convert it";
            const bool shared = settings_.backend == Backend::Wasapi && settings_.wasapi.shared;
            const int length =
                src.bitsPerSample == 1 && shared
                    ? snprintf(msg, sizeof msg,
                               "DSD%u: WASAPI shared mode plays DSD only converted to PCM; set DSD output to PCM or enable "
                               "\"convert DSD when unsupported\"",
                               src.sampleRate * 8 / 44100)
                : src.bitsPerSample == 1
                    ? snprintf(msg, sizeof msg, "DSD%u / %u ch: this device takes neither native DSD nor DoP at %.1f kHz%s",
                               src.sampleRate * 8 / 44100, src.channels, dopCarrierRate(src.sampleRate) / 1000.0,
                               settings_.convertDsdWhenUnsupported ? ""
                                                                   : "; enable \"convert DSD when unsupported\" to convert it")
                : shared ? snprintf(msg, sizeof msg,
                                    "%u Hz / %u-bit / %u ch: the WASAPI shared-mode mixer did not accept a converted stream",
                                    src.sampleRate, src.bitsPerSample, src.channels)
                         : snprintf(msg, sizeof msg, "%u Hz / %u-bit / %u ch has no bit-perfect format on this device%s",
                                    src.sampleRate, src.bitsPerSample, src.channels, hint);
            if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "No bit-perfect format available on this device");
            position_ = deviceChange ? startFrame : 0;
            return fail(Error(ErrorCode::FormatNotSupported, msg), onDeviceFailure);
        }
        if (startFrame && src.totalFrames && startFrame >= src.totalFrames) startFrame = 0;
        negotiated_ = *plan;
        plan_ = withEq(negotiated_);
        fmt_ = plan_.format;
        bpf_ = fmt_.bytesPerFrame();
        sourceRate_ = src.sampleRate;
        outputRate_ = fmt_.sampleRate;
        dec.reset(); // the Rust Engine opens its own decoder through the host ABI
        stages_[3] = measure::now();
        try {
            rust_ = std::make_unique<RustSession>(file_, decoder, fmt_, startFrame, settings_.pcmBitsByFormat, conversion(),
                                                  plan_.dsd, timing(settings_).predecodeSeconds);
            rust_->setEq(settings_.eq);
        } catch (const Error& e) {
            position_ = waitingForSource_ ? startFrame : 0;
            return fail(e, waitingForSource_ ? PlayState::Paused : PlayState::Stopped);
        }
        stages_[4] = measure::now();

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
        dspThread_ = std::thread([this] { dspMain(); });

        // Let the decoder get ahead before the device starts pulling.
        const auto startFrames = uint64_t(fmt_.sampleRate * kStartFillSeconds);
        for (int i = 0; i < 600 && bufferedFrames() < startFrames && !decodeDone_; i++) Sleep(5);
        stages_[5] = measure::now();

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
            stages_[6] = measure::now();
        } catch (const Error& e) {
            rawDsd_ = false;
            stopDecoder();
            rust_.reset();
            position_ = startFrame;
            // Device busy or unplugged: pause and wait for the user (docs/Plan.md §2)
            const bool waitForDevice = deviceChange || e.code() == ErrorCode::DeviceInUse || e.code() == ErrorCode::DeviceLost;
            if (!waitForDevice) output_.reset();
            return fail(e, waitForDevice ? PlayState::Paused : PlayState::Stopped);
        }
        active_ = true;
        waitingForSource_ = false;
        const bool raw = rawDsd_.load(std::memory_order_relaxed);
        pendingSilence_ = !lastOpened_ || lastOpened_->generation != outputGeneration_ || lastOpened_->format != fmt_ ||
                          lastOpened_->raw != raw;
        lastOpened_ = Opened{outputGeneration_, fmt_, raw};
        {
            std::lock_guard lock(snapMutex_);
            snap_.output = fmt_;
            snap_.plan = plan_;
            snap_.outputStatus = outputStatus();
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        log::info("engine: %s via %s, %s, start %llu", toUtf8(file_).c_str(), decoderName(decoder), describe(fmt_).c_str(),
                  (unsigned long long)startFrame);
        if (playNow) {
            playing_ = true;
            startRust();
            try {
                output_->start();
            } catch (const Error& e) {
                if (plan_.dsd != DsdPath::Dop) throw;
                // DoP is the user's choice; the other DSD paths stay their call (docs/Plan.md §5.3 item 4).
                throw Error(e.code(), std::string(e.what()) + "; play this file as native DSD or DSD converted to PCM instead");
            }
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

    // Keeps the short device ring filled while decoding waits for slow storage (docs/Plan.md §5.1, §5.2).
    void dspMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        try {
            while (!stopDecode_)
                if (rust_->process() == 0) Sleep(2);
        } catch (const Error& e) {
            log::error("engine: DSP error: %s", e.what());
            std::lock_guard lock(decodeErrMutex_);
            if (!decodeError_) decodeError_ = e;
        }
    }

    uint64_t bufferedFrames() const { return prefilled_.load(std::memory_order_relaxed); }

    void stopDecoder() {
        stopDecode_ = true;
        if (decodeThread_.joinable()) decodeThread_.join();
        if (dspThread_.joinable()) dspThread_.join();
    }

    void closeOutput() {
        if (output_) {
            try {
                output_->close();
            } catch (const std::exception& e) {
                log::warn("engine: closing output failed: %s", e.what());
                output_.reset();
            }
        }
    }

    void teardown() {
        closeOutput();
        rawDsd_ = false; // the output no longer renders
        leadSilence_ = 0;
        leadDone_ = false;
        pendingSilence_ = false;
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
            negotiated_ = next.plan;
            plan_ = withEq(negotiated_);
            sourceRate_ = next.source.sampleRate;
            fileDecoder_ = next.requested;
            format_ = next.format;
            const auto& source = next.source;
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.format = next.format;
            snap_.decoder = next.decoder;
            snap_.plan = plan_;
            snap_.source = source;
            snap_.md5.reset();
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
        if (err && GetFileAttributesW(file_.c_str()) == INVALID_FILE_ATTRIBUTES) {
            waitingForSource_ = true;
            return fail(Error(err->code(), std::string("Source unavailable: ") + err->what()), PlayState::Paused);
        }
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
        // A followed default device that disappears mid-playback resumes on the next default (docs/Plan.md §2).
        resumeOnDefault_ = followsDefault() && state_ == PlayState::Playing && e.code() == ErrorCode::DeviceLost;
        teardown();
        output_.reset(); // the device may have changed; reopen from scratch on the next play
        fail(e, PlayState::Paused);
    }

    bool followsDefault() const { return settings_.backend == Backend::Wasapi && settings_.wasapi.deviceId.empty(); }

    void updateWatcher() {
        if (!followsDefault()) {
            watcher_.reset();
            return;
        }
        if (watcher_) return;
        try {
            watcher_ = watcherFactory_([this] { post([this] { onDefaultDeviceChanged(); }); });
        } catch (const Error& e) {
            log::warn("engine: %s", e.what());
        }
    }

    // The Windows default render device changed while WASAPI follows it: an open stream moves to the new default and
    // keeps playing only if it was playing; playback lost with the old default resumes; otherwise the next play opens
    // the new default.
    void onDefaultDeviceChanged() {
        if (!followsDefault()) return;
        log::info("engine: Windows default output device changed");
        if (active_) {
            const bool wasPlaying = state_ == PlayState::Playing;
            const uint64_t pos = currentPosition();
            teardown();
            output_.reset();
            startSession(pos, wasPlaying, true);
        } else if (resumeOnDefault_) {
            resumeOnDefault_ = false;
            output_.reset();
            startSession(position_, true, true);
        } else {
            output_.reset();
        }
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
        if (s == PlayState::Paused) pausedAt_ = std::chrono::steady_clock::now();
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
    DeviceWatcherFactory watcherFactory_;
    std::unique_ptr<DeviceWatcher> watcher_; // while WASAPI follows the Windows default
    bool resumeOnDefault_ = false;           // the followed default was lost while playing
    bool waitingForSource_ = false;
    std::unique_ptr<OutputBridge> output_;
    std::unique_ptr<RustSession> rust_;
    std::wstring file_;
    std::optional<DecoderKind> fileDecoder_; // per-file override of the per-format setting
    std::optional<AudioFormat> format_;
    PlayState state_ = PlayState::Stopped;
    uint64_t position_ = 0;
    uint64_t session_ = 0;
    int64_t openedAt_ = 0;
    std::atomic<int64_t> firstAudio_{0};
    std::array<int64_t, 7> stages_{};
    bool firstAudioLogged_ = true;
    uint64_t loggedUnderruns_ = 0;
    uint64_t loggedLateCallbacks_ = 0;
    std::chrono::steady_clock::time_point lateCallbacksLoggedAt_;
    std::atomic<bool> active_{false};
    DeviceFormat fmt_;
    OutputPlan plan_;
    OutputPlan negotiated_;         // plan_ before the EQ, which withEq() applies
    std::string planAdjustment_;    // a fixed rate the device refused
    uint64_t outputGeneration_ = 0; // counts output devices opened; a new one starts in an unknown format
    struct Opened {
        uint64_t generation;
        DeviceFormat format;
        bool raw;
    };
    std::optional<Opened> lastOpened_;
    bool pendingSilence_ = false; // the open stream changed format and has not started yet
    std::chrono::steady_clock::time_point pausedAt_;
    std::atomic<uint32_t> sourceRate_{0}, outputRate_{0}; // read by snapshot() while the stream plays

    // shared with the decode / output threads
    size_t bpf_ = 8;
    std::atomic<uint64_t> base_{0}, consumed_{0}, prefilled_{0}, underruns_{0};
    std::atomic<bool> decodeDone_{false}, ended_{false}, stopDecode_{false}, playing_{false}, transitionPending_{false};
    std::atomic<bool> rawDsd_{false};            // the open stream is DoP or native DSD
    std::atomic<uint64_t> leadSilence_{0};       // frames of format-change silence still to play
    std::atomic<bool> leadDone_{false};          // the silence is over; the control thread starts the Engine
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
    std::thread decodeThread_, dspThread_;
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

PlaybackEngine::PlaybackEngine(const Settings& s, OutputFactory f, DeviceWatcherFactory w)
    : impl_(std::make_unique<Impl>(s, std::move(f), std::move(w))) {}
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
void PlaybackEngine::discardPreloads() {
    impl_->call<void>([this] { impl_->discardPreloads(); });
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
