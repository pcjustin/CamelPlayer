// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/PlaybackEngine.h"

#include <windows.h>
#include <objbase.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "Engine/decode/FormatProbe.h"
#include "Engine/Dsp.h"
#include "Engine/FormatRule.h"
#include "Engine/RustSession.h"
#include "Engine/SharedLoad.h"
#include "Engine/SlowSource.h"
#include "OutputBridge/OutputBridge.h"
#include "OutputBridge/OutputChoices.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"

namespace camel::audio {

namespace {
constexpr double kStartFillSeconds = 0.5; // buffered before the output starts
// Playing a slow source pauses to buffer again when less than this is buffered ahead.
constexpr double kLowWaterSeconds = 1.5;
constexpr auto kFillWindow = std::chrono::seconds(2); // how long the read rate is measured over while it plays
constexpr auto kStopDeviceAfterPause = std::chrono::seconds(2);
// A network player or AirPlay receiver takes the audio over the network a slow source is read from, and stops when
// reading leaves it too little: while it plays, the source is read at this share of the rate measured.
constexpr double kStreamReadShare = 0.4;

std::optional<SampleType> fixedSampleType(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return s.wasapi.sampleType;
    case Backend::Ks:
        return s.ks.sampleType;
    case Backend::Asio:
    case Backend::Network:
    case Backend::AirPlay:
    case Backend::HQPlayer:
        return std::nullopt; // the driver or the player dictates the type
    }
    return std::nullopt;
}

DsdOutput dsdOutput(const Settings& s) {
    if (s.backend == Backend::Network) return s.network.dsdOutput;
    if (s.backend == Backend::AirPlay) return DsdOutput::Native;     // no raw DSD, so PCM only with the permission
    if (s.backend == Backend::HQPlayer) return s.hqplayer.dsdOutput; // DSF for HQPlayer to process, or PCM if permitted
    return s.backend == Backend::Asio ? s.asio.dsdOutput : s.backend == Backend::Ks ? s.ks.dsdOutput : s.wasapi.dsdOutput;
}

uint32_t dsdPcmRate(const Settings& s) {
    if (s.backend == Backend::Network) return s.network.dsdPcmRate;
    if (s.backend == Backend::AirPlay) return 44100;
    if (s.backend == Backend::HQPlayer) return s.hqplayer.dsdPcmRate;
    return s.backend == Backend::Asio ? s.asio.dsdPcmRate : s.backend == Backend::Ks ? s.ks.dsdPcmRate : s.wasapi.dsdPcmRate;
}

// The limits an output's settings put on PCM, DoP carriers included, and on raw DSD (docs/Plan.md §6).
bool withinLimit(const Settings& s, const DeviceFormat& f) {
    if (f.type == SampleType::Dsd8) return !maxDsdByteRate(s) || f.sampleRate <= maxDsdByteRate(s);
    if (maxSampleRate(s) && f.sampleRate > maxSampleRate(s)) return false;
    return s.backend != Backend::Network || !s.network.maxBits || bytesPerSample(f.type) * 8 <= s.network.maxBits;
}

// How `src` reaches a device that takes what `supports` accepts under settings `s` (docs/Plan.md §5.3). DSD: the
// configured raw path (ASIO native, else DoP; WASAPI and KS have no native DSD), PCM only when
// chosen or allowed as a fallback. A fixed rate the device refuses falls back to the file's rate, noted in `adjustment`.
// `dspOnDsd`: the DSP acts on DSD sources, which are then modulated again at their own rate, within the output's highest
// DSD rate.
std::optional<OutputPlan> planFor(const Settings& s, const SourceInfo& src, bool lossy,
                                  const std::function<bool(const DeviceFormat&)>& deviceSupports,
                                  std::string* adjustment = nullptr, bool dspOnDsd = false) {
    const std::function<bool(const DeviceFormat&)> supports = [&](const DeviceFormat& f) {
        return withinLimit(s, f) && deviceSupports(f);
    };
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
    const uint32_t maxRate = maxSampleRate(s);
    // The set rate, else the rates upsampling tries; DSD converted to PCM is taken for a 44.1 kHz source.
    const auto chosenRates = [&](uint32_t source) {
        if (const uint32_t rate = limitedRate(fixedRate(s), maxRate)) return std::vector<uint32_t>{rate};
        return upsamplingRates(upsampling(s), upsamplingMultiple(s), source, maxRate);
    };
    // DSD upsampling: every source modulated to the first DSD rate the device takes, else the paths below.
    const DsdUpsamplingSettings dsd = dsdUpsampling(s);
    const bool native = (s.backend == Backend::Asio || s.backend == Backend::Network || s.backend == Backend::HQPlayer) &&
                        dsdOutput(s) == DsdOutput::Native;
    const auto modulated = [&](uint32_t sourceByteRate) {
        const auto rates = dsdUpsamplingRates(dsd, sourceByteRate, maxDsdByteRate(s));
        auto plan = negotiateModulated(src, rates, resampleFilter(s).modulatorOrder, native, fixedSampleType(s), supports);
        const auto dsdName = [](uint32_t byteRate) { return "DSD" + std::to_string(byteRate / 352800 * 64); };
        if (plan && adjustment) {
            const uint32_t chosen = plan->dsd == DsdPath::Dop ? plan->format.sampleRate * 2 : plan->format.sampleRate;
            if (chosen != rates.front())
                *adjustment =
                    requestedVsActive("DSD upsampling", dsdName(rates.front()), dsdName(chosen) + " (the device refused it)");
        }
        return plan;
    };
    if (src.bitsPerSample != 1) {
        if (dsd.pcm)
            if (auto plan = modulated(0)) return plan;
        const auto rates = chosenRates(src.sampleRate);
        for (const uint32_t rate : rates)
            if (auto plan = negotiateOutput(src, lossy, fixedSampleType(s), s.convertWhenUnsupported, supports, rate))
                return plan;
        auto plan = negotiateOutput(src, lossy, fixedSampleType(s), s.convertWhenUnsupported, supports, 0, maxRate);
        if (!rates.empty() && plan && adjustment)
            *adjustment =
                requestedVsActive(fixedRate(s) ? "Fixed sample rate" : "Upsampling", std::to_string(rates.front()) + " Hz",
                                  std::to_string(plan->format.sampleRate) + " Hz (the device refused it)");
        return plan;
    }
    if (auto plan = modulated(src.sampleRate)) return plan;
    if (dspOnDsd && dsd.mode == DsdUpsampling::Off) {
        std::vector<uint32_t> rates;
        for (uint32_t rate = src.sampleRate; rate >= 352800; rate /= 2)
            if (rate <= 4 * 352800 && (!maxDsdByteRate(s) || rate <= maxDsdByteRate(s))) rates.push_back(rate);
        if (auto plan = negotiateModulated(src, rates, resampleFilter(s).modulatorOrder, native, fixedSampleType(s), supports))
            return plan;
    }
    const DsdOutput mode = dsdOutput(s);
    std::optional<OutputPlan> plan;
    if (mode != DsdOutput::Pcm) plan = negotiateDsd(src, native, fixedSampleType(s), supports, maxDsdByteRate(s), maxRate);
    if (plan || (mode != DsdOutput::Pcm && !s.convertDsdWhenUnsupported)) return plan;
    for (const uint32_t rate : chosenRates(44100))
        if (auto converted = negotiateDsdToPcm(src, rate, fixedSampleType(s), s.convertWhenUnsupported, supports, maxRate, true))
            return converted;
    return negotiateDsdToPcm(src, dsdPcmRate(s), fixedSampleType(s), s.convertWhenUnsupported, supports, maxRate);
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
    case Backend::Network: // the rest is a choice of path, which reopens only when the path differs
        return a.network.udn == b.network.udn && a.network.gapless == b.network.gapless;
    case Backend::AirPlay: // the receivers come and go in the one stream (AirPlayOutput.h)
        return true;
    case Backend::HQPlayer:
        return a.hqplayer.address == b.hqplayer.address;
    }
    return false;
}

bool samePlayerPath(const NetworkSettings& a, const NetworkSettings& b) {
    return a.fixedRate == b.fixedRate && a.upsampling == b.upsampling && a.upsamplingMultiple == b.upsamplingMultiple &&
           a.filter == b.filter && a.dsdUpsampling == b.dsdUpsampling && a.dspOnDsd == b.dspOnDsd &&
           a.maxSampleRate == b.maxSampleRate && a.maxDsdRate == b.maxDsdRate && a.maxBits == b.maxBits &&
           a.dsdOutput == b.dsdOutput && a.dsdPcmRate == b.dsdPcmRate;
}

// The file's own decoder, else the per-format setting; an unsupported pairing fails instead of substituting.
DecoderKind chooseDecoder(AudioFormat format, std::optional<DecoderKind> requested, const Settings& s) {
    if (decodersFor(format).empty())
        throw Error(ErrorCode::UnsupportedStream, std::string(formatName(format)) + " is not supported yet");
    // No device takes a DTS bitstream as PCM; decoding it is a conversion (docs/Plan.md §2).
    if (format == AudioFormat::Dts && !s.convertWhenUnsupported)
        throw Error(ErrorCode::FormatNotSupported,
                    "DTS in a WAVE file has no bit-perfect format on any device; enable \"convert when unsupported\" to "
                    "decode it");
    const DecoderKind decoder = requested.value_or(decoderFor(s.decoderByFormat, format));
    if (!supports(format, decoder))
        throw Error(ErrorCode::UnsupportedStream, std::string(decoderName(decoder)) + " cannot decode " + formatName(format));
    return decoder;
}

std::atomic<int> dspStageDelayMs{0};

// After each pass of the DSP stage, which decoding runs too.
void delayDspStageForTest() {
    if (const int delay = dspStageDelayMs.load(std::memory_order_relaxed)) Sleep(DWORD(delay));
}

// The length the player shows: the exact one, or an estimate while the decoder still measures it. The snapshot's
// source keeps only the exact one, which the library records.
uint64_t shownLength(const IDecoder& decoder) {
    return decoder.info().totalFrames ? decoder.info().totalFrames : decoder.estimatedFrames();
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

    _No_competing_thread_ ~Impl() override {
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
        load::setNotPlaying(this);
        load::setShareRead(this, {}, 0.0);
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

    // As call(), for work that changes the session: an error ends the session as a posted command's does, and the caller
    // gets R{}.
    template <class R> R callGuarded(const std::function<R()>& fn) {
        return call<R>([this, &fn]() -> R {
            if constexpr (std::is_void_v<R>) {
                guarded(fn);
            } else {
                R result{};
                guarded([&] { result = fn(); });
                return result;
            }
        });
    }

    // ---- IRenderSource (real-time thread) ----
    // While a session replaces another on the open device, the device plays silence and never reaches the sessions.
    size_t render(uint8_t* dst, size_t frames) noexcept override {
        rendering_.fetch_add(1);
        size_t n = frames;
        if (switching_.load())
            memset(dst, 0, frames * bpf_);
        else
            n = renderSession(dst, frames);
        rendering_.fetch_sub(1);
        return n;
    }

    size_t renderSession(uint8_t* dst, size_t frames) noexcept {
        if (!rust_) return 0;
        if (uint64_t lead = leadSilence_.load(std::memory_order_acquire)) {
            if (pulled_.load(std::memory_order_relaxed))
                frames = size_t(std::min<uint64_t>(frames, lead)); // counted in the stream
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
        const uint64_t renderedFrames = renderedFrames_.fetch_add(n, std::memory_order_relaxed) + n;
        boundary_.store(kNoBoundary, std::memory_order_relaxed);
        if (transitions.count != renderTransitions_.load(std::memory_order_relaxed)) {
            // A queued file became audible inside this request; positions now count from its start.
            renderTransitions_.store(transitions.count, std::memory_order_release);
            if (transitions.trackPosition <= n) boundary_.store(size_t(n - transitions.trackPosition), std::memory_order_relaxed);
            // Where the track being left ends, in its own frames: base_ counts source frames, consumed_ output frames.
            leaving_.store(base_.load(std::memory_order_relaxed) +
                               toSource(consumed_.load(std::memory_order_relaxed) + n - transitions.trackPosition),
                           std::memory_order_relaxed);
            trackStartFrame_.store(renderedFrames - transitions.trackPosition, std::memory_order_relaxed);
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
            } else if (playing_.load(std::memory_order_relaxed) && !leadDone_.load(std::memory_order_acquire) &&
                       !pulled_.load(std::memory_order_relaxed)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
                if (rendered.behind) behind_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return n;
    }

    // Not into a stream another device fetches, which takes only what was rendered.
    bool writesSilence() const noexcept override {
        return rawDsd_.load(std::memory_order_acquire) && !pulled_.load(std::memory_order_relaxed);
    }
    size_t boundaryInLastRender() const noexcept override { return boundary_.load(std::memory_order_relaxed); }
    bool renderEnded() const noexcept override { return ended_.load(); }

    StreamTrack streamTrack() const override {
        std::lock_guard lock(streamMutex_);
        return streamTrack_;
    }

    std::optional<StreamTrack> nextStreamTrack() const override {
        std::lock_guard lock(streamMutex_);
        return nextStream_.empty() ? std::nullopt : std::optional(nextStream_.front());
    }

    // ---- commands (control thread) ----
    void openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb, bool afterEnd) {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
        file_ = file.wstring();
        measuredFile_.clear(); // the link may have changed since it was measured
        fileDecoder_ = decoder;
        gainDb_ = gainDb;
        fadeSkip_ = !afterEnd || !settings_.crossfade.enabled ? FadeSkip::None
                    : refusedFile_ == file_                   ? refusedSkip_
                                                              : FadeSkip::NoPreload;
        refusedFile_.clear();
        startSession(0, true, false, true);
    }

    void crossfadeTo(const std::filesystem::path& file, std::optional<DecoderKind> requested, double gainDb) {
        const bool fades = settings_.crossfade.enabled && active_ && state_ == PlayState::Playing && !rawDsd_ &&
                           settings_.backend != Backend::Network && settings_.backend != Backend::HQPlayer &&
                           !leadSilence_.load(std::memory_order_acquire);
        const auto next = fades ? prepareNext(file, requested, gainDb) : std::nullopt;
        if (!next || !rust_->crossfadeTo(file, next->decoder, gainDb, {crossfadeMs(), next->convert})) {
            if (fades)
                log::info("engine: %s opens without a crossfade: %s", toUtf8(file.wstring()).c_str(),
                          next ? "nothing plays that it can be mixed with" : "it cannot follow in the open format");
            return openAndPlay(file, requested, gainDb, false);
        }
        resumeOnDefault_ = false;
        waitingForSource_ = false;
        queued_.clear();
        {
            // What was queued goes; a stream output hears of this file at the crossfade's seam instead.
            std::lock_guard lock(streamMutex_);
            nextStream_.clear();
            nextStream_.push_back(streamTrackOf(file, *next));
        }
        // Not the next transition: a queued track whose start is already on its way to the device is heard before it.
        manualTransition_ = rust_->decodingTrack();
        heldUntil_.store(manualTransition_, std::memory_order_release);
        fadeSkip_ = FadeSkip::None;
        adopt(
            {file.wstring(), next->source, next->shown, next->format, requested, next->decoder, next->plan, next->lossy, gainDb});
        publishQueue();
    }

    void load(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb) {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
        teardown();
        file_ = file.wstring();
        measuredFile_.clear();
        fileDecoder_ = decoder;
        gainDb_ = gainDb;
        format_.reset();
        position_ = 0;
        std::optional<SourceInfo> source;
        uint64_t total = 0;
        try {
            format_ = probeFormat(file);
            const auto opened = openDecoder(chooseDecoder(*format_, decoder, settings_), file);
            source = opened->info();
            total = shownLength(*opened);
        } catch (const Error& e) {
            log::info("engine: %s is reported when playback starts (%s)", toUtf8(file_).c_str(), e.what());
        }
        {
            std::lock_guard lock(snapMutex_);
            snap_.file = file_;
            snap_.source = source;
            snap_.md5.reset();
            snap_.total = total;
            snap_.format = format_;
            snap_.decoder = displayedDecoder(settings_);
            snap_.error.reset();
            snap_.errorDetail.clear();
        }
        setState(PlayState::Stopped);
    }

    void play() {
        buffering_ = false;
        giveWay_ = false;
        if (active_) {
            if (state_ == PlayState::Paused) {
                if (!openDeferredOutput()) return;
                startRust();
                if (!std::exchange(runsPaused_, false)) output_->start();
                {
                    std::lock_guard lock(snapMutex_);
                    if (snap_.error == ErrorCode::CannotKeepUp) {
                        snap_.error.reset();
                        snap_.errorDetail.clear();
                    }
                }
                setState(PlayState::Playing);
            }
            return;
        }
        if (!file_.empty()) startSession(position_, true);
    }

    void pause() {
        resumeOnDefault_ = false;
        if (buffering_.exchange(false)) publish();
        if (active_ && state_ == PlayState::Playing) {
            // The Engine falls silent at once. A device plays that silence for a while before it stops, as a driver can
            // take half a second to stop (the DAVE's ASIO driver) and a play meanwhile would wait for it; a network
            // player is told at once.
            playing_ = false;
            leadSilence_ = 0;
            leadDone_ = false;
            rust_->pause();
            setState(PlayState::Paused);
            if (isStreamed(settings_.backend))
                output_->pause();
            else
                runsPaused_ = true;
        }
    }

    void stopDeviceOncePaused() {
        if (!runsPaused_ || std::chrono::steady_clock::now() - pausedAt_ < kStopDeviceAfterPause) return;
        runsPaused_ = false;
        output_->pause();
    }

    void stop() {
        resumeOnDefault_ = false;
        waitingForSource_ = false;
        teardown();
        position_ = 0;
        setState(PlayState::Stopped);
    }

    // A restarted session plays on when it played, or was buffering to play; one the user paused stays paused.
    bool playsOn() const { return state_ == PlayState::Playing || buffering_; }

    void seek(uint64_t frame) {
        if (active_)
            startSession(frame, playsOn(), false, true);
        else {
            position_ = frame;
            publish();
        }
    }

    // The rate the DSP chain runs at: the device's, or the 352.8 kHz a modulated DSD stream is made from.
    static uint32_t dspRate(const OutputPlan& plan) { return plan.modulatorOrder ? 352800 : plan.format.sampleRate; }

    // The DSP chain for `plan`'s output, with the impulse response at its rate; nothing for raw DSD, which bypasses it.
    // A response that cannot be used leaves convolution off.
    DspChain chainFor(const OutputPlan& plan) const { return chainFor(plan, settings_); }

    DspChain chainFor(const OutputPlan& plan, const Settings& settings) const {
        if ((plan.dsd == DsdPath::Native || plan.dsd == DsdPath::Dop) && !plan.modulatorOrder)
            return dspChain(Settings{}, nullptr);
        Impulse impulse;
        const auto& file = settings.convolution.file;
        if (settings.convolution.enabled && !file.empty() && !impulseFailedLately(file)) {
            try {
                impulse = loadImpulse(toWide(file), dspRate(plan)).impulse;
            } catch (const ImpulseError& e) {
                log::warn("impulse response: %s", e.what());
                std::lock_guard lock(impulseMutex_);
                impulseFailed_ = file;
                impulseFailedAt_ = std::chrono::steady_clock::now();
            }
        }
        return dspChain(settings, std::move(impulse));
    }

    // A response on a share that is offline takes half a minute to fail, at every track change: one that failed is
    // tried again after kImpulseRetry, or at once when the convolution settings change.
    bool impulseFailedLately(const std::string& file) const {
        std::lock_guard lock(impulseMutex_);
        return file == impulseFailed_ && std::chrono::steady_clock::now() - impulseFailedAt_ < kImpulseRetry;
    }

    // Whether settings `s` send a DSD source with ReplayGain `gainDb` through the DSP: the output's switch is on and the
    // chain, the gain or the software volume changes the signal.
    bool dspActsOnDsd(const Settings& s, double gainDb) const {
        if (!dspOnDsd(s)) return false;
        OutputPlan modulated;
        modulated.format = {352800, 2, SampleType::Dsd8};
        modulated.dsd = DsdPath::Native;
        modulated.modulatorOrder = 7;
        return gainDb != 0.0 || volumeDb(s.volume) != 0.0 || !actingNodes(chainFor(modulated, s), 352800, 2).empty();
    }

    // `plan` as the track's ReplayGain and the DSP chain leave it: processed while either changes the signal. Raw DSD
    // bypasses both, and crossfades; modulated DSD takes both, but no crossfade.
    OutputPlan withDsp(OutputPlan plan) const {
        const bool dsdStream = plan.dsd == DsdPath::Native || plan.dsd == DsdPath::Dop;
        const bool raw = dsdStream && !plan.modulatorOrder;
        plan.dsp = actingNodes(chainFor(plan), dspRate(plan), plan.format.channels);
        plan.gainDb = raw ? 0.0 : gainDb_;
        plan.muted = !raw && settings_.volume.mode == VolumeMode::Software && settings_.volume.muted;
        plan.volumeDb = raw || plan.muted ? 0.0 : volumeDb(settings_.volume);
        plan.crossfadeSeconds = settings_.crossfade.enabled && !dsdStream ? settings_.crossfade.seconds : 0.0;
        plan.fadeSkip = settings_.crossfade.enabled && dsdStream ? FadeSkip::RawDsd : fadeSkip_;
        if (!plan.dsp.empty() || plan.gainDb != 0.0 || plan.volumeDb != 0.0 || plan.muted) plan.state = PipelineState::Processed;
        return plan;
    }

    // The chain changes without reopening anything: the Engine fades to it, and the path shows it.
    void applyDsp() {
        if (!active_ || !rust_) return;
        try {
            rust_->setDsp(chainFor(negotiated_));
        } catch (const Error& e) {
            log::warn("engine: %s", e.what());
        }
        refreshPlan();
    }

    void refreshPlan() {
        plan_ = withDsp(negotiated_);
        std::lock_guard lock(snapMutex_);
        if (snap_.plan) snap_.plan = plan_;
    }

    void applySettings(const Settings& s) {
        const bool outputChanged = !sameBackendConfig(settings_, s);
        const bool chainChanged = settings_.eq != s.eq || settings_.crossfeed != s.crossfeed ||
                                  settings_.convolution != s.convolution || settings_.dspOrder != s.dspOrder;
        if (settings_.convolution != s.convolution) {
            std::lock_guard lock(impulseMutex_);
            impulseFailed_.clear();
        }
        const bool dspChanged = chainChanged || volumeDb(settings_.volume) != volumeDb(s.volume);
        // A conversion setting reopens the stream only when the path it would choose differs; so does the DSP where it
        // takes a DSD source from raw DSD to modulated DSD or back.
        const bool conversionChanged =
            (dspChanged || settings_.convertWhenUnsupported != s.convertWhenUnsupported ||
             settings_.resampleQuality != s.resampleQuality ||
             settings_.convertDsdWhenUnsupported != s.convertDsdWhenUnsupported ||
             !samePlayerPath(settings_.network, s.network) || settings_.hqplayer.dsdOutput != s.hqplayer.dsdOutput ||
             settings_.hqplayer.dsdPcmRate != s.hqplayer.dsdPcmRate) &&
            active_ && !outputChanged && conversionPathChanges(s);
        const bool decoderChanged = displayedDecoder(settings_) != displayedDecoder(s) || conversionChanged;
        const bool crossfadeChanged = settings_.crossfade != s.crossfade;
        const bool wasActive = active_;
        const bool wasPlaying = playsOn();
        const uint64_t pos = currentPosition();
        // What a network player holds cannot change, so a new chain is heard only from a new stream. A volume is heard
        // once what HQPlayer holds has played, rather than after the silence of a new stream.
        const bool restart = outputChanged || decoderChanged ||
                             (chainChanged && (s.backend == Backend::Network || s.backend == Backend::HQPlayer));
        if (restart && wasActive) teardown();
        if (outputChanged) output_.reset();
        settings_ = s;
        updateWatcher();
        {
            std::lock_guard lock(snapMutex_);
            snap_.backend = s.backend;
            snap_.decoder = displayedDecoder(s);
        }
        if (restart && wasActive) {
            startSession(pos, wasPlaying);
        } else {
            if (dspChanged)
                applyDsp();
            else if (crossfadeChanged && active_)
                refreshPlan();
            publish();
        }
    }

    bool queueNext(const std::filesystem::path& file, std::optional<DecoderKind> requested, double gainDb, uint32_t fadeMs) {
        if (!active_ || rust_->transitions().nextQueued || !output_->gapless() || streamsSlowSource()) return false;
        const auto next = prepareNext(file, requested, gainDb);
        if (!next || !rust_->queueNext(file, next->decoder, gainDb, {fadeMs, next->convert})) return false;
        queued_.push_back(
            {file.wstring(), next->source, next->shown, next->format, requested, next->decoder, next->plan, next->lossy, gainDb});
        {
            std::lock_guard lock(streamMutex_);
            nextStream_.push_back(streamTrackOf(file, *next));
        }
        publishQueue();
        return true;
    }

    struct Prepared {
        AudioFormat format{};
        DecoderKind decoder = DecoderKind::Sndfile;
        SourceInfo source;
        uint64_t shown = 0; // shownLength()
        OutputPlan plan;
        bool lossy = false;   // decoded from a lossy source, or as float
        bool convert = false; // to the open format, which crossfade permits
    };

    // `file` as it would follow in the open stream: in the same output format, or with crossfade on converted to it
    // (docs/Plan.md §5.4). Nullopt when it cannot be opened or take that format; a format is remembered for the path.
    std::optional<Prepared> prepareNext(const std::filesystem::path& file, std::optional<DecoderKind> requested, double gainDb) {
        Prepared next;
        bool& lossy = next.lossy;
        try {
            next.format = probeFormat(file);
            next.decoder = chooseDecoder(next.format, requested, settings_);
            const auto opened = openDecoder(next.decoder, file);
            next.source = opened->info();
            next.shown = shownLength(*opened);
            lossy = opened->readsFloat() || isLossy(next.format);
        } catch (const Error&) {
            return std::nullopt; // reported when the file is opened normally
        }
        std::optional<OutputPlan> plan;
        try {
            plan = negotiate(next.source, lossy, gainDb);
        } catch (const Error&) {
            return std::nullopt; // the device question fails again when the file is opened normally
        }
        // The open stream modulates every track or none.
        if (!plan || plan->format != fmt_ || plan->modulatorOrder != negotiated_.modulatorOrder) {
            plan = settings_.crossfade.enabled ? intoOpenFormat(next.source, lossy) : std::nullopt;
            if (!plan) {
                refusedFile_ = file.wstring();
                refusedSkip_ = rawDsd_ || next.source.bitsPerSample == 1 ? FadeSkip::RawDsd : FadeSkip::Format;
                return std::nullopt;
            }
            next.convert = true;
        }
        next.plan = *plan;
        return next;
    }

    // `next` as a stream output is told of it, its length in the stream's frames.
    _Requires_lock_held_(streamMutex_) StreamTrack streamTrackOf(const std::filesystem::path& file, const Prepared& next) {
        const uint32_t in = next.source.sampleRate, out = fmt_.sampleRate;
        return {file.wstring(), in == out || !in ? next.shown : (next.shown * out + in / 2) / in, 0, ++streamSequence_};
    }

    // `src` converted to the open stream's format; nullopt for a raw DSD stream, or DSD without permission to become PCM.
    std::optional<OutputPlan> intoOpenFormat(const SourceInfo& src, bool lossy) const {
        if (rawDsd_) return std::nullopt;
        const auto open = [this](const DeviceFormat& f) { return f == fmt_; };
        std::optional<OutputPlan> plan;
        if (src.bitsPerSample != 1)
            plan = negotiateOutput(src, lossy, fmt_.type, true, open, fmt_.sampleRate);
        else if (dsdOutput(settings_) == DsdOutput::Pcm || settings_.convertDsdWhenUnsupported)
            plan = negotiateDsdToPcm(src, fmt_.sampleRate, fmt_.type, true, open);
        if (plan && plan_.shared) {
            plan->shared = true;
            plan->state = PipelineState::Processed;
        }
        return plan;
    }

    uint32_t crossfadeMs() const { return uint32_t(std::lround(settings_.crossfade.seconds * 1000.0)); }

    // A crossfade holds a track's last seconds back until the next one is decoded behind them.
    uint32_t predecodeSeconds() const {
        const uint32_t configured = timing(settings_).predecodeSeconds;
        if (!settings_.crossfade.enabled) return configured;
        return std::max(configured, uint32_t(std::ceil(settings_.crossfade.seconds)) + 2);
    }

    bool clearNext() {
        if (!rust_ || !rust_->transitions().nextQueued) return true;
        if (!rust_->clearNext()) return false;
        queued_.pop_back();
        {
            std::lock_guard lock(streamMutex_);
            if (!nextStream_.empty()) nextStream_.pop_back();
        }
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
        const bool playing = playsOn();
        teardown();
        startSession(position, playing);
    }

    // A panel that cannot open is no playback failure: its error goes to the caller.
    bool showControlPanel() {
        if (settings_.backend != Backend::Asio) return false;
        ensureOutput();
        return output_->showControlPanel();
    }

    EngineSnapshot snapshot() const {
        std::lock_guard lock(snapMutex_);
        EngineSnapshot s = snap_;
        if (active_) {
            s.position = sourcePosition();
            s.underruns = underruns_.load(std::memory_order_relaxed);
            const auto sinceFull =
                std::chrono::steady_clock::now().time_since_epoch().count() - fullAt_.load(std::memory_order_relaxed);
            s.ahead = s.state == PlayState::Playing &&
                      (decodeDone_.load(std::memory_order_acquire) ||
                       sinceFull < std::chrono::steady_clock::duration(std::chrono::milliseconds(250)).count());
        }
        return s;
    }

    void setListener(std::function<void()> l) {
        std::lock_guard lock(snapMutex_);
        listener_ = std::move(l);
    }

    bool waitForState(PlayState s, std::chrono::milliseconds timeout) {
        std::unique_lock lock(snapMutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (snap_.state != s)
            if (snapCv_.wait_until(lock, deadline) == std::cv_status::timeout) return snap_.state == s;
        return true;
    }

private:
    void controlMain() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock lock(qMutex_);
                const auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                while (!quit_ && queue_.empty() && !eventDue() && !leadDone_.load())
                    if (qCv_.wait_until(lock, due) == std::cv_status::timeout) break;
                if (!queue_.empty()) {
                    fn = std::move(queue_.front());
                    queue_.pop_front();
                } else if (quit_) {
                    break;
                }
            }
            if (fn) guarded(fn);
            guarded([this] { follow(); });
        }
        CoUninitialize();
    }

    // An exception leaving this thread would end the program: it ends the session instead.
    template <typename Work> void guarded(const Work& work) {
        try {
            work();
        } catch (const Error& e) {
            teardown();
            output_.reset();
            fail(e,
                 e.code() == ErrorCode::DeviceLost || e.code() == ErrorCode::DeviceInUse || e.code() == ErrorCode::DeviceChanged
                     ? PlayState::Paused
                     : PlayState::Stopped);
        } catch (const std::exception& e) {
            teardown();
            output_.reset();
            fail(Error(ErrorCode::Other, e.what()), PlayState::Stopped);
        }
    }

    // The session's work between commands.
    void follow() {
        if (active_ && leadDone_.load()) {
            if (state_ == PlayState::Playing) rust_->play();
            leadDone_ = false;
        }
        if (active_ && transitionPending_.load() && !stillHeld()) {
            transitionPending_ = false;
            onTransition();
        }
        if (active_ && ended_.load() && !transitionPending_.load() && heldFrames() == 0) onEnded();
        if (active_) {
            logFirstAudio();
            logUnderruns();
            if (!active_ || !keepsUp()) return;
            if (giveWay_.exchange(false) && state_ == PlayState::Playing) giveWay();
            reportAudible();
            refreshNextSlot();
            refreshMd5();
            refreshTotal();
            followLoad();
            checkBuffering();
        }
        if (active_ && state_ == PlayState::Paused) stopDeviceOncePaused();
        if (active_ && state_ == PlayState::Paused && !buffering_) releaseAfterPause();
    }

    // The decoder empties the slot when it moves on to a queued file, seconds before that file is heard; the Server
    // polls this and queues the one after it.
    void refreshNextSlot() {
        // Reading a slow source's next file would share the network with the file being buffered, or with the player the
        // audio goes to (streamsSlowSource()).
        const bool free = !rust_->transitions().nextQueued && !buffering_ && !streamsSlowSource();
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

    // A length the decoder learned while the track plays replaces the estimate. It is read between two counts of the
    // Engine's transitions, which this thread has handled up to, so it belongs to the file the snapshot shows.
    void refreshTotal() {
        if (manualTransition_ > handledTransitions_) return; // the snapshot shows a file not heard yet
        const uint64_t before = rust_->transitions().count;
        const uint64_t total = rust_->totalFrames();
        if (!total || before != handledTransitions_ || rust_->transitions().count != before) return;
        {
            std::lock_guard lock(snapMutex_);
            if (!snap_.source || snap_.source->totalFrames == total) return;
            snap_.source->totalFrames = total;
            snap_.total = total;
        }
        publish();
    }

    // A stream the DSP stage falls behind on plays in gaps. While other engines play, the one started last among those
    // upsampling or modulating to DSD gives way (Engine/SharedLoad.h); alone, a modulated DSD stream stops, for a lower
    // rate to be chosen.
    bool keepsUp() {
        if (behind_.load(std::memory_order_relaxed) < kBehindLimit) return true;
        if (load::ranShort(this) == load::Shortfall::GivingWay || !plan_.modulatorOrder) {
            behind_ = 0;
            return true;
        }
        const uint32_t byteRate = plan_.dsd == DsdPath::Dop ? plan_.format.sampleRate * 2 : plan_.format.sampleRate;
        const uint64_t at = currentPosition();
        teardown();
        position_ = at;
        fail(Error(ErrorCode::TooSlow, "this computer cannot modulate to DSD" + std::to_string(byteRate / 352800 * 64) +
                                           " in real time; choose a lower DSD rate"),
             PlayState::Stopped);
        return false;
    }

    // Dropouts are otherwise only a counter in the status page, which a new track resets. A source that stalls underruns
    // on every render, so they are told at most once a second.
    void logUnderruns() {
        const uint64_t count = underruns_.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (count <= loggedUnderruns_ || now - underrunsLoggedAt_ < std::chrono::seconds(1)) return;
        underrunsLoggedAt_ = now;
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
        held_.store(status.heldFrames, std::memory_order_relaxed);
        const uint64_t rendered = renderedFrames_.load(std::memory_order_relaxed);
        heard_.store(rendered - std::min(status.heldFrames, rendered), std::memory_order_relaxed);
        rust_->reportAudible(toOutput(base_) + consumed - std::min(buffered, consumed));
    }

    uint64_t heldFrames() const { return output_ ? output_->status().heldFrames : 0; }

    // The end of a track and a change of track wait for what a network player still holds.
    bool eventDue() const {
        const uint64_t held = held_.load(std::memory_order_relaxed);
        return (ended_.load() && !held) || (transitionPending_.load() && held <= consumed_.load(std::memory_order_relaxed));
    }

    // A queued track reaches the device long before it is heard on a network player.
    bool stillHeld() const { return heldFrames() > consumed_.load(std::memory_order_relaxed); }

    uint64_t currentPosition() const { return active_ ? sourcePosition() : position_; }

    // The device consumes output frames; positions count source frames, which differ when the stream is resampled. A
    // manual crossfade's file stays at its start until its fade is heard.
    uint64_t sourcePosition() const {
        if (renderTransitions_.load(std::memory_order_acquire) < heldUntil_.load(std::memory_order_acquire)) return 0;
        uint64_t heard = consumed_.load(std::memory_order_relaxed);
        if (pulled_.load(std::memory_order_relaxed)) {
            // Rendering runs in bursts past the tracks' boundaries, so what was heard counts from the session's start, which
            // the boundaries do not move.
            const uint64_t measured = heard_.load(std::memory_order_relaxed);
            const uint64_t trackStart = trackStartFrame_.load(std::memory_order_relaxed);
            if (transitionPending_.load(std::memory_order_acquire)) {
                const uint64_t left = trackStart - std::min(measured, trackStart); // of the track being left, still heard
                const uint64_t end = leaving_.load(std::memory_order_relaxed);
                return end - std::min(toSource(left), end);
            }
            heard = measured - std::min(measured, trackStart);
        }
        return base_ + toSource(heard);
    }
    uint64_t toSource(uint64_t outputFrames) const noexcept {
        const uint32_t in = sourceRate_.load(std::memory_order_relaxed), out = outputRate_.load(std::memory_order_relaxed);
        return in == out || !out ? outputFrames : (outputFrames * in + out / 2) / out;
    }
    uint64_t toOutput(uint64_t sourceFrames) const {
        const uint32_t in = sourceRate_.load(std::memory_order_relaxed), out = outputRate_.load(std::memory_order_relaxed);
        return in == out || !in ? sourceFrames : (sourceFrames * out + in / 2) / in;
    }

    std::optional<OutputPlan> negotiate(const SourceInfo& src, bool lossy, double gainDb, std::string* adjustment = nullptr) {
        return planFor(
            settings_, src, lossy, [&](const DeviceFormat& f) { return output_->supports(f); }, adjustment,
            src.bitsPerSample == 1 && dspActsOnDsd(settings_, gainDb));
    }

    RustSession::Conversion conversion() const {
        if (plan_.shared) return {true, resampleFilter(settings_), plan_.dsd == DsdPath::Pcm};
        if (plan_.modulatorOrder) return {true, resampleFilter(settings_), true, plan_.modulatorOrder};
        // A fixed rate is the permission to resample to it, a highest rate to resample above it and to modulate DSD
        // again below it (docs/Plan.md §6).
        const bool chosenResample =
            plan_.dsd == DsdPath::None && plan_.resample &&
            (fixedRate(settings_) || upsampling(settings_) != Upsampling::Off || maxSampleRate(settings_));
        return {settings_.convertWhenUnsupported || chosenResample, resampleFilter(settings_),
                plan_.dsd == DsdPath::Pcm || plan_.remodulate};
    }

    bool takesNoPcm() {
        for (const uint32_t rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U})
            for (const SampleType type :
                 {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32, SampleType::Float32})
                if (output_->supports({rate, 2, type})) return false;
        return true;
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
        if (const uint64_t frames = silenceFrames())
            leadSilence_ = frames;
        else
            rust_->play();
        pendingSilence_ = false;
    }

    uint64_t silenceFrames() const {
        const uint32_t ms = timing(settings_).formatChangeSilenceMs;
        return pendingSilence_ && ms ? std::max<uint64_t>(1, uint64_t(fmt_.sampleRate) * ms / 1000) : 0;
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

    // Its next file is read only once it ends, as reading it would take the network from the stream.
    bool streamsSlowSource() const { return readRate_ && isStreamed(settings_.backend); }

    // The bytes per second `file` plays at, from its size and length; 0 when either is unknown.
    static double playedBytesPerSecond(const std::wstring& file, uint64_t frames, uint32_t rate) {
        std::error_code error;
        const uint64_t bytes = std::filesystem::file_size(file, error);
        const double seconds = framesToSeconds(frames, rate);
        return error || !bytes || seconds <= 0 ? 0.0 : double(bytes) / seconds;
    }

    // A file on a network share that plays at kMeasuredBytesPerSecond or more is read for a moment first; one read slower
    // than it plays gets a ring that can hold what the rest of it needs buffered, and play waits for that. Returns the
    // seconds to predecode.
    uint32_t measureSource(const SourceInfo& src, uint64_t shown, uint64_t startFrame) {
        readRate_.reset();
        streamRate_ = 0;
        streamPace_ = 0;
        sharePace_ = 0;
        shareRead_ = 0;
        share_.clear();
        load::setShareRead(this, {}, 0.0);
        const uint32_t configured = predecodeSeconds();
        if (!shown || !onNetworkShare(file_)) return configured;
        const double needed = playedBytesPerSecond(file_, shown, src.sampleRate);
        share_ = shareOf(file_);
        shareNeeded_ = needed;
        shareRead_ = needed;
        // A share serves the engines already reading it first (docs/Plan.md §5.12): this one reads what they leave.
        const double others = load::othersRead(this, share_);
        if (needed < kMeasuredBytesPerSecond) return configured;
        const uint64_t from = std::min(startFrame, shown);
        std::error_code error;
        const uint64_t size = std::filesystem::file_size(file_, error);
        const uint64_t at = error ? 0 : uint64_t(double(size) * double(from) / double(shown));
        // A seek in the file measured when it started uses that rate: measuring takes up to 4 MB from the share, as it
        // would from the others reading it, whose rate was measured when the first of them started.
        const bool kept = measuredFile_ == file_ && measuredRate_;
        const auto known = others > 0 ? load::shareRate(share_) : std::nullopt;
        const auto measured = kept    ? measuredRate_
                              : known ? known
                                      : measureReadRate(file_, at, std::clamp<uint64_t>(uint64_t(needed), 1u << 20, 4u << 20));
        measuredFile_ = file_;
        measuredRate_ = measured;
        if (!measured) return configured;
        load::setShareRate(share_, *measured);
        const auto read = std::optional(others > 0 ? std::max(*measured - others, *measured * kLeastShare) : *measured);
        shareRead_ = std::min(needed, *read);
        if (others > 0) sharePace_ = *read / needed;
        const double left = framesToSeconds(shown - from, src.sampleRate);
        const bool streamed = isStreamed(settings_.backend);
        const double wait = bufferSeconds(left, *read / needed) <= 0
                                ? 0.0
                                : bufferSeconds(left, *read * (streamed ? kStreamReadShare : 1.0) / needed);
        log::info("engine: %s reads at %.2f MB/s%s, plays at %.2f MB/s%s", toUtf8(file_).c_str(), *read / 1e6,
                  kept         ? " (as measured when it started)"
                  : others > 0 ? " (what the others reading its share leave)"
                               : "",
                  needed / 1e6,
                  wait <= 0  ? ""
                  : streamed ? "; buffering before it plays, then reading it slower while it streams"
                             : "; buffering before it plays");
        if (wait <= 0) return configured;
        readRate_ = *read;
        streamRate_ = streamed ? *read * kStreamReadShare : 0.0;
        streamPace_ = streamRate_ / needed;
        neededRate_ = needed;
        neededFor_ = file_;
        fillAt_ = {};
        // The decoded ring stores a DSD source converted to PCM as DSD bytes, more per second than the PCM it sizes for.
        double scale = 1.0;
        if (src.bitsPerSample == 1 && plan_.dsd != DsdPath::Native && plan_.dsd != DsdPath::Dop && fmt_.sampleRate)
            scale = std::max(1.0, double(src.sampleRate) * src.channels / (double(fmt_.sampleRate) * fmt_.channels * 4.0));
        return uint32_t(std::ceil((wait + configured) * scale));
    }

    // What this engine asks of the computer beside the others (Engine/SharedLoad.h): whether it upsamples or modulates
    // while it plays, and what it reads of its share while it reads, at no more than the readers before it leave.
    void followLoad() {
        if (state_ == PlayState::Playing)
            load::setPlaying(this, upsamples(), [this] {
                giveWay_ = true;
                qCv_.notify_all();
            });
        if (share_.empty()) return;
        const bool reading = (state_ == PlayState::Playing || buffering_) && !decodeDone_.load(std::memory_order_acquire);
        load::setShareRead(this, share_, reading ? shareRead_ : 0.0);
        if (sharePace_.load(std::memory_order_relaxed) <= 0) return;
        const auto measured = load::shareRate(share_);
        const double others = load::othersRead(this, share_);
        if (!measured || others <= 0) {
            log::info("engine: %s reads its share alone now", toUtf8(file_).c_str());
            sharePace_ = 0;
            return;
        }
        const double left = std::max(*measured - others, *measured * kLeastShare);
        sharePace_ = left / shareNeeded_;
        if (readRate_ && left < *readRate_) readRate_ = left;
    }

    bool upsamples() const {
        return plan_.modulatorOrder || plan_.remodulate ||
               (plan_.resample && outputRate_.load(std::memory_order_relaxed) > sourceRate_.load(std::memory_order_relaxed));
    }

    // Another engine ran short and this one, started last among those upsampling or modulating, pauses for the others.
    void giveWay() {
        log::warn("engine: pausing %s, as this computer cannot keep up with its upsampling beside the others playing",
                  toUtf8(file_).c_str());
        pause();
        {
            std::lock_guard lock(snapMutex_);
            snap_.error = ErrorCode::CannotKeepUp;
            snap_.errorDetail = "this computer cannot keep up with this zone's upsampling while the others play";
        }
        publish();
    }

    // While a slow source is buffered: plays once enough is buffered to reach the end of the file, or when the ring is
    // full. While it plays: pauses to buffer again when the buffer runs low, as when a queued file reads slower than the
    // one measured.
    void checkBuffering() {
        if (!readRate_ || !rust_) return;
        const uint32_t in = sourceRate_.load(std::memory_order_relaxed), out = outputRate_.load(std::memory_order_relaxed);
        const auto b = rust_->buffered();
        const bool decodingNext = queued_.size() > (rust_->transitions().nextQueued ? 1u : 0u);
        const double decoded = framesToSeconds(b.decoded, in), submitted = framesToSeconds(b.submitted, out);
        if (decodingNext || decodeDone_.load(std::memory_order_acquire) || decoded < submitted || !in || !out) {
            fillAt_ = {}; // the rest of the audible file is decoded
            if (buffering_) resumeBuffered();
            return;
        }
        if (neededFor_ != file_) {
            neededFor_ = file_;
            if (const double needed = playedBytesPerSecond(file_, b.total, in); needed > 0) neededRate_ = needed;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool full = now.time_since_epoch().count() - fullAt_.load(std::memory_order_relaxed) <
                          std::chrono::steady_clock::duration(std::chrono::milliseconds(250)).count();
        if (fillAt_ == std::chrono::steady_clock::time_point{} || full) {
            fillAt_ = now;
            fillFrom_ = b.decoded;
        } else if (now - fillAt_ >= kFillWindow && b.decoded >= fillFrom_) {
            const double rate =
                framesToSeconds(b.decoded - fillFrom_, in) / std::chrono::duration<double>(now - fillAt_).count() * neededRate_;
            readRate_ = *readRate_ * 0.7 + rate * 0.3;
            fillAt_ = now;
            fillFrom_ = b.decoded;
        }
        // Buffered at the rate it reads, played while it reads no faster than a stream leaves it.
        const double ratio = *readRate_ / neededRate_;
        const double playRatio = (streamRate_ > 0 ? std::min(*readRate_, streamRate_) : *readRate_) / neededRate_;
        const double ahead = decoded - submitted;
        const double left = std::max(0.0, framesToSeconds(b.total, in) - submitted);
        const double wanted = bufferSeconds(left, playRatio);
        if (buffering_) {
            if (ahead >= wanted || full) return resumeBuffered();
            const auto shownSeconds = uint32_t(std::ceil((wanted - ahead) / std::max(ratio, 0.05)));
            if (shownSeconds != bufferingShown_) {
                bufferingShown_ = shownSeconds;
                publish();
            }
        } else if (state_ == PlayState::Playing && ahead < kLowWaterSeconds && left > ahead + 1.0 && wanted > 0) {
            log::info("engine: %.1f s buffered at %.1f s of %s; buffering again", ahead, submitted, toUtf8(file_).c_str());
            pause();
            buffering_ = true;
            bufferingShown_ = uint32_t(std::ceil(wanted / std::max(ratio, 0.05)));
            publish();
        }
    }

    void resumeBuffered() {
        log::info("engine: buffered %s", toUtf8(file_).c_str());
        buffering_ = false;
        if (state_ == PlayState::Paused)
            play();
        else
            publish();
    }

    // Opens the network player a buffered session kept waiting; false when it failed and the session ended paused.
    bool openDeferredOutput() {
        if (outputOpen_) return true;
        try {
            output_->open(fmt_, this);
            outputOpen_ = true;
            return true;
        } catch (const Error& e) {
            const uint64_t at = sourcePosition();
            teardown();
            position_ = at;
            fail(e, PlayState::Paused);
            return false;
        }
    }

    // `deviceChange`: reopening after the Windows default changed; a device that cannot play pauses instead of ending the
    // track, so the user can resume (docs/Plan.md §2). `keepOutput`: a manual change or a seek, which plays on the open
    // device when the file takes its format.
    void startSession(uint64_t startFrame, bool playNow, bool deviceChange = false, bool keepOutput = false) {
        const PlayState onDeviceFailure = deviceChange ? PlayState::Paused : PlayState::Stopped;
        if (!std::exchange(choosingAgain_, false)) refusedAtStart_ = 0;
        teardown(keepOutput && playNow && !deviceChange);
        // A session that fails before it plays leaves no device open behind its silence.
        struct ReleaseKept {
            Impl* engine;
            ~ReleaseKept() {
                if (engine->switching_) engine->closeOutput();
            }
        } releaseKept{this};
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
        DecoderKind decoder = DecoderKind::Sndfile;
        try {
            format_.reset();
            format_ = probeFormat(file_);
            stages_[0] = measure::now();
            decoder = chooseDecoder(*format_, fileDecoder_, settings_);
            dec = openDecoder(decoder, file_, *format_);
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
            snap_.total = shownLength(*dec);
            snap_.output.reset();
            snap_.plan.reset();
        }
        // ADPCM decodes to exact 16-bit samples, which still come from a lossy source.
        const bool lossy = dec->readsFloat() || isLossy(*format_);
        std::optional<OutputPlan> plan;
        planAdjustment_.clear();
        try {
            ensureOutput();
            plan = negotiate(src, lossy, gainDb_, &planAdjustment_);
            // The open device plays on only in the PCM format it plays; any other choice is made with it closed, as a
            // running device may refuse formats it would take.
            if (switching_ && (!plan || !lastOpened_ || plan->dsd == DsdPath::Native || plan->dsd == DsdPath::Dop ||
                               withDsp(*plan).format != lastOpened_->format)) {
                closeOutput();
                planAdjustment_.clear();
                plan = negotiate(src, lossy, gainDb_, &planAdjustment_);
            }
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
            // A driver that takes several formats and starts none, as Chord's ASIO driver does while WASAPI or Kernel
            // Streaming holds its DAC (docs/Task-breakdown.md MZ-R00), is held by something else.
            if (refusedAtStart_ >= 2) {
                position_ = startFrame;
                return fail(Error(ErrorCode::DeviceInUse, "the device took " + std::to_string(refusedAtStart_) +
                                                              " formats and could start none: another program may be using it"),
                            PlayState::Paused);
            }
            // Chord's ASIO driver refuses every PCM rate while another program holds it without playing (HQPlayer
            // stopped); what it said of DSD is remembered from before.
            if (settings_.backend == Backend::Asio && src.bitsPerSample != 1 && takesNoPcm()) {
                position_ = startFrame;
                return fail(Error(ErrorCode::DeviceInUse, "the ASIO driver takes no format: another program may be using it"),
                            PlayState::Paused);
            }
            position_ = deviceChange ? startFrame : 0;
            return fail(Error(ErrorCode::FormatNotSupported, msg), onDeviceFailure);
        }
        const uint64_t shown = shownLength(*dec);
        if (startFrame && shown && startFrame >= shown) startFrame = 0;
        negotiated_ = *plan;
        negotiatedLossy_ = lossy;
        plan_ = withDsp(negotiated_);
        fmt_ = plan_.format;
        bpf_ = fmt_.bytesPerFrame();
        sourceRate_ = src.sampleRate;
        outputRate_ = fmt_.sampleRate;
        const uint32_t predecode = measureSource(src, shown, startFrame);
        if (readRate_ && playNow) {
            playNow = false; // play() follows once checkBuffering() finds enough buffered
            buffering_ = true;
            bufferingShown_ = 0;
        }
        stages_[3] = measure::now();
        try {
            // The Rust Engine reads, through the host ABI, the decoder opened here.
            rust_ = std::make_unique<RustSession>(file_, decoder, fmt_, startFrame, conversion(), plan_.dsd, predecode,
                                                  plan_.gainDb, std::move(dec));
            rust_->setDsp(chainFor(plan_));
        } catch (const Error& e) {
            position_ = waitingForSource_ ? startFrame : 0;
            return fail(e, waitingForSource_ ? PlayState::Paused : PlayState::Stopped);
        }
        stages_[4] = measure::now();

        {
            std::lock_guard lock(streamMutex_);
            streamTrack_ = {file_, shown > startFrame ? toOutput(shown - startFrame) : 0, toOutput(startFrame),
                            ++streamSequence_};
            nextStream_.clear();
        }
        held_ = 0;
        heard_ = 0;
        renderedFrames_ = 0;
        trackStartFrame_ = 0;
        pulled_ = isStreamed(settings_.backend);
        base_ = startFrame;
        consumed_ = 0;
        prefilled_ = 0;
        fullAt_ = 0;
        underruns_ = 0;
        behind_ = 0;
        decodeDone_ = false;
        ended_ = false;
        stopDecode_ = false;
        renderTransitions_ = 0;
        handledTransitions_ = 0;
        manualTransition_ = 0;
        heldUntil_ = 0;
        transitionPending_ = false;
        {
            std::lock_guard lock(decodeErrMutex_);
            decodeError_.reset();
        }
        decodeThread_ = std::thread([this] { decodeMain(); });
        dspThread_ = std::thread([this] { dspMain(); });

        // Let the decoder get ahead before the device starts pulling.
        const auto startFrames = uint64_t(fmt_.sampleRate * kStartFillSeconds);
        for (int i = 0; i < 600 && !buffering_ && bufferedFrames() < startFrames && !decodeDone_; i++) Sleep(5);
        stages_[5] = measure::now();

        // A kept device would play silence until the buffer is ready; it opens again when playback starts.
        if (buffering_ && switching_) closeOutput();
        // The handlers report to the session the device plays for, which a kept device changes while it runs.
        const bool kept = switching_;
        outputSession_ = session;
        if (!kept) {
            output_->setErrorHandler([this](const Error& e) {
                post([this, owner = outputSession_.load(), e] {
                    if (owner == session_ && active_) onOutputError(e);
                });
            });
            output_->setRestartHandler([this] {
                post([this, owner = outputSession_.load()] {
                    if (owner == session_ && active_) onRestart();
                });
            });
        }
        rawDsd_.store(plan_.dsd == DsdPath::Native || plan_.dsd == DsdPath::Dop, std::memory_order_release);
        const bool raw = rawDsd_.load(std::memory_order_relaxed);
        pendingSilence_ = !lastOpened_ || lastOpened_->generation != outputGeneration_ || lastOpened_->format != fmt_ ||
                          lastOpened_->raw != raw;
        if (pulled_) {
            std::lock_guard lock(streamMutex_);
            streamTrack_.leadFrames = silenceFrames();
        }
        try {
            // A network player hears of the track only when it is to play it, not minutes earlier while the source is
            // read (openDeferredOutput()).
            if (!buffering_ || !isStreamed(settings_.backend)) {
                if (!kept) output_->open(fmt_, this);
                outputOpen_ = true;
            }
            stages_[6] = measure::now();
        } catch (const Error& e) {
            rawDsd_ = false;
            stopDecoder();
            rust_.reset();
            position_ = startFrame;
            // Device busy or unplugged: pause and wait for the user (docs/Plan.md §2)
            const bool waitForDevice = deviceChange || e.code() == ErrorCode::DeviceInUse || e.code() == ErrorCode::DeviceLost ||
                                       e.code() == ErrorCode::DeviceChanged;
            if (!waitForDevice) output_.reset();
            return fail(e, waitForDevice ? PlayState::Paused : PlayState::Stopped);
        }
        active_ = true;
        waitingForSource_ = false;
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
                if (kept)
                    switching_ = false; // the running device reaches this session from its next request
                else
                    output_->start();
            } catch (const Error& e) {
                // The driver accepted the format but cannot run it, and now refuses it: choose again, which converts
                // only with permission (docs/Plan.md §5.3).
                if (e.code() == ErrorCode::DriverError && plan_.dsd == DsdPath::None && !output_->supports(fmt_)) {
                    log::warn("engine: %s; choosing another format", e.what());
                    ++refusedAtStart_;
                    choosingAgain_ = true;
                    return startSession(startFrame, playNow, deviceChange);
                }
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
        struct Paced {
            std::chrono::steady_clock::time_point at;
            double decoded, pace;
        };
        std::optional<Paced> pacedFrom;
        try {
            while (!stopDecode_) {
                const double stream = playing_ ? streamPace_.load(std::memory_order_relaxed) : 0.0;
                const double share = sharePace_.load(std::memory_order_relaxed);
                if (const double pace = stream > 0 && share > 0 ? std::min(stream, share) : std::max(stream, share); pace > 0) {
                    const auto now = std::chrono::steady_clock::now();
                    const double decoded =
                        framesToSeconds(rust_->buffered().decoded, sourceRate_.load(std::memory_order_relaxed));
                    if (!pacedFrom || pacedFrom->pace != pace) pacedFrom = Paced{now, decoded, pace};
                    if (decoded - pacedFrom->decoded > std::chrono::duration<double>(now - pacedFrom->at).count() * pace) {
                        Sleep(20);
                        continue;
                    }
                } else {
                    pacedFrom.reset();
                }
                uint64_t frames = 0;
                const auto result = rust_->prefill(frames);
                delayDspStageForTest();
                if (result == RustSession::Prefill::End) {
                    // Keep polling: a file queued while the tail still plays continues the stream gaplessly.
                    decodeDone_.store(true, std::memory_order_release);
                    Sleep(5);
                    continue;
                }
                if (result == RustSession::Prefill::Full) {
                    fullAt_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
                    pacedFrom.reset(); // caught up, so a pause in reading is not made up for afterwards
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
            while (!stopDecode_) {
                if (rust_->process() == 0) Sleep(2);
                delayDspStageForTest();
            }
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
        outputOpen_ = false;
        runsPaused_ = false;
        switching_ = false; // a closed device renders nothing
    }

    // Whether the open device can play on into another session: a running PCM stream, which silence does not disturb, on
    // the device instance that opened it.
    bool canKeepOutput() const {
        return output_ && outputOpen_ && state_ == PlayState::Playing && !rawDsd_.load(std::memory_order_acquire) &&
               lastOpened_ && lastOpened_->generation == outputGeneration_;
    }

    // `keepOutput`: the device plays silence on while another session is prepared, which reuses it if it takes the
    // same format; closing and reopening an ASIO driver costs about half a second at every manual change.
    void teardown(bool keepOutput = false) {
        if (keepOutput && !isStreamed(settings_.backend) && canKeepOutput()) { // a network player starts anew
            switching_ = true;
            while (rendering_.load()) std::this_thread::yield();
        } else {
            closeOutput();
        }
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
        readRate_.reset();
        streamRate_ = 0;
        streamPace_ = 0;
        sharePace_ = 0;
        share_.clear();
        load::setShareRead(this, {}, 0.0);
        load::setNotPlaying(this);
        buffering_ = false;
        std::lock_guard lock(snapMutex_);
        snap_.output.reset();
        snap_.plan.reset();
        snap_.queuedFiles = 0;
        snap_.nextSlotFree = false;
    }

    struct Queued {
        std::wstring file;
        SourceInfo source;
        uint64_t shown = 0; // shownLength()
        AudioFormat format;
        std::optional<DecoderKind> requested;
        DecoderKind decoder;
        OutputPlan plan;
        bool lossy = false;
        double gainDb = 0.0;
    };

    // Queued files are now audible: the last of them becomes the current file without reopening the device.
    void onTransition() {
        const uint64_t audible = renderTransitions_.load(std::memory_order_acquire);
        if (audible == handledTransitions_) return;
        while (handledTransitions_ < audible) {
            ++handledTransitions_;
            // A manual crossfade made its file current when it was asked for; its seam is now past.
            if (handledTransitions_ == manualTransition_) {
                std::lock_guard lock(streamMutex_);
                if (!nextStream_.empty()) nextStream_.pop_front();
                continue;
            }
            if (queued_.empty()) continue;
            const Queued next = queued_.front();
            queued_.pop_front();
            {
                std::lock_guard lock(streamMutex_);
                if (!nextStream_.empty()) nextStream_.pop_front();
            }
            fadeSkip_ = FadeSkip::None;
            adopt(next);
            std::lock_guard lock(snapMutex_);
            snap_.queuedFiles = queued_.size();
            snap_.transitions++;
        }
        publishQueue();
    }

    void adopt(const Queued& next) {
        file_ = next.file;
        gainDb_ = next.gainDb;
        negotiated_ = next.plan;
        negotiatedLossy_ = next.lossy;
        plan_ = withDsp(negotiated_);
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
        snap_.total = next.shown;
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
            const bool wasPlaying = playsOn();
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
        const bool wasPlaying = playsOn();
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
        if (s == PlayState::Playing)
            followLoad();
        else
            load::setNotPlaying(this);
        publish();
    }

    void publish() {
        std::function<void()> l;
        {
            std::lock_guard lock(snapMutex_);
            snap_.state = state_;
            snap_.position = currentPosition();
            snap_.bufferingSeconds = buffering_ ? std::optional(bufferingShown_.load()) : std::nullopt;
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
        if (plan_.resample && resampleFilter(settings_) != resampleFilter(s)) return true;
        const auto source = snapshot().source;
        if (!source) return false;
        // As the stream negotiated it, before the DSP chain, ReplayGain, volume and crossfade decorate it.
        const auto plan = planFor(
            s, *source, negotiatedLossy_, [&](const DeviceFormat& f) { return output_->supports(f); }, nullptr,
            source->bitsPerSample == 1 && dspActsOnDsd(s, gainDb_));
        return plan != negotiated_;
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
    bool outputOpen_ = false;                // output_ holds an open stream
    std::atomic<uint64_t> outputSession_{0}; // the session the open stream plays for
    std::atomic<bool> switching_{false};     // the open stream plays silence while another session is prepared
    std::atomic<int> rendering_{0};          // device requests inside render()
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
    std::chrono::steady_clock::time_point underrunsLoggedAt_;
    uint64_t loggedLateCallbacks_ = 0;
    // Formats the device took and then could not start, since the last start the user asked for.
    size_t refusedAtStart_ = 0;
    bool choosingAgain_ = false;
    std::chrono::steady_clock::time_point lateCallbacksLoggedAt_;
    std::atomic<bool> active_{false};
    DeviceFormat fmt_;
    OutputPlan plan_;
    OutputPlan negotiated_;         // plan_ before the DSP chain, which withDsp() applies
    bool negotiatedLossy_ = false;  // what negotiated_ took the source for
    double gainDb_ = 0.0;           // the current file's ReplayGain
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
    bool runsPaused_ = false; // paused, with the device still playing silence until stopDeviceOncePaused()
    std::atomic<uint32_t> sourceRate_{0}, outputRate_{0}; // read by snapshot() while the stream plays

    // shared with the decode / output threads
    size_t bpf_ = 8;
    std::atomic<uint64_t> base_{0}, consumed_{0}, prefilled_{0}, underruns_{0};
    std::atomic<uint64_t> behind_{0}; // underruns while decoded audio waited
    static constexpr uint64_t kBehindLimit = 3;
    std::atomic<bool> pulled_{false}; // a network player takes what is ready, so nothing short is an underrun
    std::atomic<uint64_t> held_{0};   // frames rendered that the device has not played
    std::atomic<uint64_t> heard_{0};  // frames a network player has played since the session started
    std::atomic<uint64_t> renderedFrames_{0}, trackStartFrame_{0}; // since the session started
    std::atomic<uint64_t> leaving_{0}; // where the track being left ended, while a network player still plays it
    std::atomic<size_t> boundary_{kNoBoundary};
    mutable std::mutex streamMutex_;
    _Guarded_by_(streamMutex_) StreamTrack streamTrack_;
    _Guarded_by_(streamMutex_) std::deque<StreamTrack> nextStream_; // queued, in order
    _Guarded_by_(streamMutex_) uint64_t streamSequence_ = 0;
    std::atomic<int64_t> fullAt_{0}; // steady_clock ticks when the decoder last found its ring full
    std::atomic<bool> decodeDone_{false}, ended_{false}, stopDecode_{false}, playing_{false}, transitionPending_{false};
    std::atomic<bool> rawDsd_{false};            // the open stream is DoP or native DSD
    std::atomic<uint64_t> leadSilence_{0};       // frames of format-change silence still to play
    std::atomic<bool> leadDone_{false};          // the silence is over; the control thread starts the Engine
    std::atomic<uint64_t> renderTransitions_{0}; // written by the output callback
    uint64_t handledTransitions_ = 0;
    uint64_t manualTransition_ = 0;      // the transition that makes a manual crossfade's file audible
    std::atomic<uint64_t> heldUntil_{0}; // its position stays 0 until that transition
    FadeSkip fadeSkip_ = FadeSkip::None; // why the change into the current file did not crossfade
    std::wstring refusedFile_;           // the last file that could not follow in the open format, and why
    FadeSkip refusedSkip_ = FadeSkip::None;
    std::deque<Queued> queued_; // queued files not yet audible: those decoding has reached, then the one in the slot
    // A source read slower than it plays (Engine/SlowSource.h).
    std::optional<double> readRate_; // bytes per second: measured, then followed while the ring fills
    std::wstring measuredFile_;      // the file measureReadRate() last measured, at measuredRate_
    std::optional<double> measuredRate_;
    double neededRate_ = 0;             // bytes per second neededFor_ plays at
    double streamRate_ = 0;             // bytes per second it is read at while it streams; 0 when it does not
    std::atomic<double> streamPace_{0}; // seconds of it decoded per second while it streams; 0 when unpaced
    // The server the file is read from (Engine/SharedLoad.h), empty for a local file; what it plays at and what this
    // engine reads of it while it reads.
    std::wstring share_;
    double shareNeeded_ = 0, shareRead_ = 0;
    std::atomic<double> sharePace_{0}; // seconds of it decoded per second, leaving the readers before it theirs; 0 unpaced
    static constexpr double kLeastShare = 0.01; // of the share's rate, read however much the others read
    std::atomic<bool> giveWay_{false};          // asked to pause for the other engines' upsampling
    static constexpr auto kImpulseRetry = std::chrono::minutes(10);
    mutable std::mutex impulseMutex_;
    _Guarded_by_(impulseMutex_) mutable std::string impulseFailed_; // the response file that could not be read
    _Guarded_by_(impulseMutex_) mutable std::chrono::steady_clock::time_point impulseFailedAt_;
    std::wstring neededFor_;
    std::chrono::steady_clock::time_point fillAt_; // where the read rate is being measured from
    uint64_t fillFrom_ = 0;
    std::atomic<bool> buffering_{false}; // paused until enough is buffered ahead
    std::atomic<uint32_t> bufferingShown_{0};
    std::thread decodeThread_, dspThread_;
    std::mutex decodeErrMutex_;
    _Guarded_by_(decodeErrMutex_) std::optional<Error> decodeError_;

    std::thread thread_;
    std::mutex qMutex_;
    std::condition_variable qCv_;
    _Guarded_by_(qMutex_) std::deque<std::function<void()>> queue_;
    _Guarded_by_(qMutex_) bool quit_ = false;

    mutable std::mutex snapMutex_;
    std::condition_variable snapCv_;
    _Guarded_by_(snapMutex_) EngineSnapshot snap_;
    _Guarded_by_(snapMutex_) std::function<void()> listener_;
};

PlaybackEngine::PlaybackEngine(const Settings& s, OutputFactory f, DeviceWatcherFactory w)
    : impl_(std::make_unique<Impl>(s, std::move(f), std::move(w))) {}
PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::openAndPlay(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb,
                                 bool afterEnd) {
    auto fileCopy = std::make_shared<std::filesystem::path>(file);
    impl_->post([this, fileCopy, decoder, gainDb, afterEnd] { impl_->openAndPlay(*fileCopy, decoder, gainDb, afterEnd); });
}
void PlaybackEngine::crossfadeTo(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb) {
    auto fileCopy = std::make_shared<std::filesystem::path>(file);
    impl_->post([this, fileCopy, decoder, gainDb] { impl_->crossfadeTo(*fileCopy, decoder, gainDb); });
}
void PlaybackEngine::load(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb) {
    auto fileCopy = std::make_shared<std::filesystem::path>(file);
    impl_->post([this, fileCopy, decoder, gainDb] { impl_->load(*fileCopy, decoder, gainDb); });
}
void PlaybackEngine::play() {
    impl_->post([this] { impl_->play(); });
}
void PlaybackEngine::pause() {
    impl_->post([this] { impl_->pause(); });
}
void PlaybackEngine::togglePause() {
    impl_->post([this] {
        // Waiting for the buffer counts as playing: the button that pauses stops the wait.
        if (const auto s = impl_->snapshot(); s.state == PlayState::Playing || s.bufferingSeconds)
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
bool PlaybackEngine::queueNext(const std::filesystem::path& file, std::optional<DecoderKind> decoder, double gainDb,
                               uint32_t crossfadeMs) {
    return impl_->callGuarded<bool>(
        [this, &file, decoder, gainDb, crossfadeMs] { return impl_->queueNext(file, decoder, gainDb, crossfadeMs); });
}
bool PlaybackEngine::clearNext() {
    return impl_->callGuarded<bool>([this] { return impl_->clearNext(); });
}
void PlaybackEngine::discardPreloads() {
    impl_->callGuarded<void>([this] { impl_->discardPreloads(); });
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
void PlaybackEngine::waitIdle(std::chrono::milliseconds period, const std::function<void()>& meanwhile) {
    auto idle = std::make_shared<std::promise<void>>();
    const auto done = idle->get_future();
    impl_->post([idle] { idle->set_value(); });
    while (done.wait_for(period) != std::future_status::ready) meanwhile();
}
bool PlaybackEngine::waitForState(PlayState s, std::chrono::milliseconds t) {
    return impl_->waitForState(s, t);
}

void setDspStageDelayForTest(std::chrono::milliseconds delay) {
    dspStageDelayMs.store(int(delay.count()), std::memory_order_relaxed);
}

} // namespace camel::audio
