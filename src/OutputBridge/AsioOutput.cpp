#include "OutputBridge/AsioOutput.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "OutputBridge/AsioAbi.h"
#include "OutputBridge/AsioLayout.h"
#include "OutputBridge/OutputChoices.h"

namespace camel::audio {
namespace {

std::optional<SampleType> mapSampleType(asio::SampleType t) {
    switch (t) {
    case asio::sample::kInt16Lsb:
        return SampleType::Int16;
    case asio::sample::kInt24Lsb:
        return SampleType::Int24;
    case asio::sample::kInt32Lsb:
        return SampleType::Int32;
    default:
        return std::nullopt; // float and LSB-justified 32-bit types are not bit-perfect targets in the MVP
    }
}

const char* asioTypeName(asio::SampleType t) {
    switch (t) {
    case asio::sample::kInt16Lsb:
        return "Int16LSB";
    case asio::sample::kInt24Lsb:
        return "Int24LSB";
    case asio::sample::kInt32Lsb:
        return "Int32LSB";
    case asio::sample::kFloat32Lsb:
        return "Float32LSB";
    case asio::sample::kFloat64Lsb:
        return "Float64LSB";
    case asio::sample::kInt32Lsb16:
        return "Int32LSB16";
    case asio::sample::kInt32Lsb18:
        return "Int32LSB18";
    case asio::sample::kInt32Lsb20:
        return "Int32LSB20";
    case asio::sample::kInt32Lsb24:
        return "Int32LSB24";
    case asio::sample::kDsdInt8Lsb1:
        return "DSDInt8LSB1";
    case asio::sample::kDsdInt8Msb1:
        return "DSDInt8MSB1";
    case asio::sample::kDsdInt8Ner8:
        return "DSDInt8NER8";
    default:
        return "other";
    }
}

class AsioOutput;
std::atomic<AsioOutput*> g_active{nullptr}; // ASIO callbacks carry no user pointer (§5.6)

class AsioOutput final : public IOutput {
public:
    explicit AsioOutput(const AsioSettings& s) : cfg_(s) {
        thread_ = std::thread([this] { staMain(); });
        std::unique_lock lock(qMutex_);
        qCv_.wait(lock, [&] { return hwnd_ != nullptr || staFailed_; });
    }

    ~AsioOutput() override {
        try {
            close();
        } catch (...) {
            OutputDebugStringW(L"ASIO: failed to close driver during destruction.\n");
        }
        try {
            invoke([this] { unload(); });
        } catch (...) {
            OutputDebugStringW(L"ASIO: failed to unload driver during destruction.\n");
        }
        {
            std::lock_guard lock(qMutex_);
            quit_ = true;
        }
        PostThreadMessageW(threadId_, WM_NULL, 0, 0);
        thread_.join();
    }

    bool supports(const DeviceFormat& f) override {
        bool ok = false;
        try {
            invoke([&] {
                load();
                if (f.type == SampleType::Dsd8)
                    ok = f.channels == 2 && supportsDsd(f.sampleRate);
                else if (driverType_ && mapSampleType(*driverType_) == f.type && f.channels == 2)
                    ok = drv_->canSampleRate(asio::SampleRate(f.sampleRate)) == asio::kOk;
            });
        } catch (const Error&) {
            return true; // let open() report why the driver is unusable
        }
        return ok;
    }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        close();
        invoke([&] { openOnSta(f, source); });
    }

    void start() override {
        invoke([&] {
            if (buffersCreated_ && !started_) {
                // Some drivers accept a rate in canSampleRate/setSampleRate that they cannot run (Chord ASIO at 1411.2 kHz).
                if (drv_->start() != asio::kOk)
                    throw Error(ErrorCode::DriverError,
                                "ASIO driver accepted " + describe(fmt_) + " but could not start it (ASIOStart failed)");
                started_ = true;
            }
        });
    }

    void pause() override {
        invoke([&] {
            if (started_) {
                drv_->stop();
                started_ = false;
            }
        });
    }

    void close() override {
        invoke([&] {
            if (started_) drv_->stop();
            started_ = false;
            if (buffersCreated_) drv_->disposeBuffers();
            buffersCreated_ = false;
            if (dsdMode_) setIoFormat(asio::IoFormatType::Pcm); // leave the driver in PCM mode for the next stream
            source_.store(nullptr);
            AsioOutput* self = this;
            g_active.compare_exchange_strong(self, nullptr);
        });
    }

    OutputStatus status() const override {
        std::lock_guard lock(statusMutex_);
        OutputStatus s = status_;
        // The measured period checks how the driver counts DSD buffer sizes.
        if (const uint64_t us = callbackPeriodUs_.load(std::memory_order_relaxed)) {
            char period[64];
            const int length = snprintf(period, sizeof period, ", DSD callback period %.2f ms", double(us) / 1000.0);
            if (length > 0 && size_t(length) < sizeof period) s.detail += period;
        }
        return s;
    }

    bool controlPanel() {
        bool ok = false;
        invoke([&] {
            load();
            ok = drv_->controlPanel() == asio::kOk;
        });
        return ok;
    }

    // --- driver callbacks (driver thread) ---
    void onBufferSwitch(long index) noexcept {
        IRenderSource* src = source_.load(std::memory_order_acquire);
        const size_t frames = bufferFrames_;
        const uint32_t bps = bytesPerSample(fmt_.type);
        size_t got = src ? src->render(interleaved_.data(), frames) : 0;
        if (got < frames) memset(interleaved_.data() + got * 2 * bps, dsdMode_ ? kDsdSilence : 0, (frames - got) * 2 * bps);
        splitStereo(interleaved_.data(), frames, bps, dsdMode_ && *driverType_ == asio::sample::kDsdInt8Lsb1,
                    static_cast<uint8_t*>(buffers_[0].buffers[index]), static_cast<uint8_t*>(buffers_[1].buffers[index]));
        if (outputReady_) drv_->outputReady();
        if (dsdMode_ && callbacks_ < kTimedCallbacks) {
            ++callbacks_;
            if (callbacks_ == 1) {
                firstCallback_ = std::chrono::steady_clock::now();
            } else if (callbacks_ == kTimedCallbacks) {
                const auto elapsed = std::chrono::steady_clock::now() - firstCallback_;
                callbackPeriodUs_ =
                    uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) / (kTimedCallbacks - 1);
            }
        }
    }
    long onMessage(long selector, long value) noexcept {
        switch (selector) {
        case asio::message::kSelectorSupported:
            return value == asio::message::kResetRequest || value == asio::message::kEngineVersion ||
                   value == asio::message::kResyncRequest || value == asio::message::kLatenciesChanged ||
                   value == asio::message::kSupportsTimeInfo || value == asio::message::kBufferSizeChange;
        case asio::message::kEngineVersion:
            return 2;
        case asio::message::kResetRequest:
        case asio::message::kBufferSizeChange:
            log::info("asio: driver requested reset (%ld)", selector);
            raiseRestart();
            return 1;
        case asio::message::kResyncRequest:
        case asio::message::kLatenciesChanged:
        case asio::message::kSupportsTimeInfo:
            return 1;
        default:
            return 0;
        }
    }
    void onRateChanged() noexcept { raiseRestart(); }

private:
    // Runs `fn` on the STA thread and waits; exceptions propagate to the caller.
    void invoke(std::function<void()> fn) {
        if (std::this_thread::get_id() == thread_.get_id()) {
            fn();
            return;
        }
        std::packaged_task<void()> task(std::move(fn));
        auto done = task.get_future();
        {
            std::lock_guard lock(qMutex_);
            queue_.push_back(std::move(task));
        }
        PostThreadMessageW(threadId_, WM_NULL, 0, 0);
        done.get();
    }

    void staMain() {
        threadId_ = GetCurrentThreadId();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the message queue
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"CamelPlayer ASIO", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        {
            std::lock_guard lock(qMutex_);
            hwnd_ = hwnd;
            staFailed_ = hwnd == nullptr;
            qCv_.notify_all();
        }
        for (;;) {
            std::deque<std::packaged_task<void()>> tasks;
            {
                std::lock_guard lock(qMutex_);
                tasks.swap(queue_);
                if (quit_ && tasks.empty()) break;
            }
            for (auto& t : tasks) t();
            if (!tasks.empty()) continue;
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (hwnd) DestroyWindow(hwnd);
        CoUninitialize();
    }

    void load() {
        if (drv_) return;
        CLSID clsid{};
        std::string id = cfg_.driverClsid;
        const auto drivers = listAsioDrivers();
        if (id.empty()) {
            if (drivers.empty()) throw Error(ErrorCode::DeviceNotFound, "no ASIO driver installed");
            id = drivers.front().id;
        } else if (std::none_of(drivers.begin(), drivers.end(), [&](const DeviceEntry& d) {
                       return d.id == id && d.name.find("(unavailable)") == std::string::npos;
                   })) {
            // A saved driver is never replaced by another one.
            throw Error(ErrorCode::DeviceNotFound, "ASIO driver not connected (not installed)");
        }
        if (FAILED(CLSIDFromString(toWide(id).c_str(), &clsid))) throw Error(ErrorCode::DeviceNotFound, "bad ASIO driver id");
        asio::Driver* d = nullptr;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&d));
        if (FAILED(hr) || !d) {
            char msg[96];
            const int length = snprintf(msg, sizeof msg, "cannot load ASIO driver (0x%08lX)", (unsigned long)hr);
            if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "cannot load ASIO driver");
            throw Error(ErrorCode::DriverError, msg);
        }
        if (!d->init(hwnd_)) {
            char err[128] = {};
            d->getErrorMessage(err);
            d->Release();
            // Drivers fail init when their device is unplugged.
            throw Error(ErrorCode::DeviceNotFound, std::string("ASIO device not connected: ") + err);
        }
        drv_ = d;
        char name[64] = {};
        drv_->getDriverName(name);
        driverName_ = name;
        long ins = 0, outs = 0;
        drv_->getChannels(&ins, &outs);
        numOutputs_ = outs;
        channels_ = asioChannels(cfg_.outLeft, cfg_.outRight, outs);
        driverType_ = outputType();
        log::info("asio: loaded %s (%ld outputs, %s)", name, outs, driverType_ ? asioTypeName(*driverType_) : "?");
    }

    void unload() {
        if (!drv_) return;
        if (started_) drv_->stop();
        if (buffersCreated_) drv_->disposeBuffers();
        started_ = buffersCreated_ = false;
        drv_->Release();
        drv_ = nullptr;
        driverType_.reset();
        dsdMode_ = false;
        dsdCaps_.clear();
    }

    bool setIoFormat(asio::IoFormatType type) {
        asio::IoFormat format{};
        format.type = type;
        const bool ok = drv_->future(asio::future::kSetIoFormat, &format) == asio::kSuccess;
        if (ok) dsdMode_ = type == asio::IoFormatType::Dsd;
        return ok;
    }

    std::optional<asio::SampleType> outputType() {
        asio::ChannelInfo ci{};
        ci.channel = channels_.left;
        ci.isInput = asio::kFalse;
        if (channels_.left < numOutputs_ && drv_->getChannelInfo(&ci) == asio::kOk) return ci.type;
        return std::nullopt;
    }

    // Native DSD at `byteFrameRate` * 8 samples per second, with a bit-per-sample layout the Engine can fill. Probing
    // switches the driver to DSD mode and back, so an open stream answers only for its own format.
    bool supportsDsd(uint32_t byteFrameRate) {
        if (const auto it = dsdCaps_.find(byteFrameRate); it != dsdCaps_.end()) return it->second;
        if (buffersCreated_) return fmt_ == DeviceFormat{byteFrameRate, 2, SampleType::Dsd8};
        asio::IoFormat query{};
        query.type = asio::IoFormatType::Dsd;
        bool ok = drv_->future(asio::future::kCanDoIoFormat, &query) == asio::kSuccess && setIoFormat(asio::IoFormatType::Dsd);
        if (ok) {
            const auto type = outputType();
            ok = (type == asio::sample::kDsdInt8Msb1 || type == asio::sample::kDsdInt8Lsb1) &&
                 drv_->canSampleRate(asio::SampleRate(double(byteFrameRate) * 8)) == asio::kOk;
            setIoFormat(asio::IoFormatType::Pcm);
        }
        dsdCaps_[byteFrameRate] = ok;
        return ok;
    }

    void openOnSta(const DeviceFormat& f, IRenderSource* source) {
        load();
        if (numOutputs_ < 2) throw Error(ErrorCode::DeviceNotFound, "ASIO driver has fewer than two outputs");
        const bool dsd = f.type == SampleType::Dsd8;
        if (dsd != dsdMode_ && !setIoFormat(dsd ? asio::IoFormatType::Dsd : asio::IoFormatType::Pcm))
            throw Error(ErrorCode::FormatNotSupported, dsd ? "ASIO driver refused DSD mode" : "ASIO driver refused PCM mode");
        driverType_ = outputType();
        const bool typeOk = dsd ? driverType_ == asio::sample::kDsdInt8Msb1 || driverType_ == asio::sample::kDsdInt8Lsb1
                                : driverType_ && mapSampleType(*driverType_) == f.type;
        if (!typeOk) throw Error(ErrorCode::FormatNotSupported, "ASIO sample type mismatch");
        // DSD rates are set in DSD samples per second (2.8224 MHz for DSD64); a device frame holds eight of them.
        const asio::SampleRate rate = dsd ? double(f.sampleRate) * 8 : double(f.sampleRate);
        asio::SampleRate cur = 0;
        drv_->getSampleRate(&cur);
        if (cur != rate && drv_->setSampleRate(rate) != asio::kOk)
            throw Error(ErrorCode::FormatNotSupported, "ASIOSetSampleRate failed");
        long minS = 0, maxS = 0, pref = 0, gran = 0;
        drv_->getBufferSize(&minS, &maxS, &pref, &gran);
        const AsioBufferChoice buffer = asioBufferSize(cfg_.bufferSize, minS, maxS, pref, gran);
        const long size = buffer.size;
        std::vector<std::string> adjustments;
        if (buffer.adjusted)
            adjustments.push_back(requestedVsActive("ASIO buffer", std::to_string(cfg_.bufferSize),
                                                    std::to_string(size) + " (driver min " + std::to_string(minS) + " / max " +
                                                        std::to_string(maxS) + " / granularity " + std::to_string(gran) + ")"));
        if (channels_.adjusted)
            adjustments.push_back(requestedVsActive("ASIO output channels",
                                                    std::to_string(cfg_.outLeft + 1) + "/" + std::to_string(cfg_.outRight + 1),
                                                    "1/2 (driver has " + std::to_string(numOutputs_) + " outputs)"));
        buffers_[0] = {asio::kFalse, channels_.left, {nullptr, nullptr}};
        buffers_[1] = {asio::kFalse, channels_.right, {nullptr, nullptr}};
        static asio::Callbacks callbacks = {
            [](long index, asio::Bool) {
                if (auto* active = g_active.load(std::memory_order_acquire)) active->onBufferSwitch(index);
            },
            [](asio::SampleRate) {
                if (auto* active = g_active.load(std::memory_order_acquire)) active->onRateChanged();
            },
            [](long selector, long value, void*, double*) -> long {
                auto* active = g_active.load(std::memory_order_acquire);
                return active ? active->onMessage(selector, value) : 0;
            },
            [](asio::Time* t, long index, asio::Bool) -> asio::Time* {
                if (auto* active = g_active.load(std::memory_order_acquire)) active->onBufferSwitch(index);
                return t;
            },
        };
        fmt_ = f;
        bufferSize_ = size;
        // In DSD mode the driver counts buffer sizes in DSD samples, eight per byte (docs/Plan.md §5.6).
        bufferFrames_ = dsd ? size_t(size) / 8 : size_t(size);
        callbacks_ = 0;
        callbackPeriodUs_ = 0;
        interleaved_.assign(bufferFrames_ * f.bytesPerFrame(), dsd ? kDsdSilence : 0);
        g_active = this;
        if (drv_->createBuffers(buffers_, 2, size, &callbacks) != asio::kOk) {
            g_active = nullptr;
            throw Error(ErrorCode::DriverError, "ASIOCreateBuffers failed");
        }
        buffersCreated_ = true;
        outputReady_ = drv_->outputReady() == asio::kOk;
        source_.store(source, std::memory_order_release);
        long inLat = 0, outLat = 0;
        drv_->getLatencies(&inLat, &outLat);
        std::lock_guard lock(statusMutex_);
        status_.mode = "ASIO · " + driverName_ + (dsd ? " · native DSD" : "");
        status_.bufferFrames = uint32_t(bufferFrames_);
        status_.adjustments = std::move(adjustments);
        char d[160];
        const int length =
            snprintf(d, sizeof d, "%s, buffer %ld (min %ld / max %ld), output latency %ld%s", asioTypeName(*driverType_), size,
                     minS, maxS, outLat, outLat == 0 ? " (driver reports 0)" : "");
        status_.detail = length < 0 || size_t(length) >= sizeof d ? "ASIO buffer status unavailable" : d;
    }

    AsioSettings cfg_;
    std::thread thread_;
    DWORD threadId_ = 0;
    HWND hwnd_ = nullptr;
    bool staFailed_ = false, quit_ = false;
    std::mutex qMutex_;
    std::condition_variable qCv_;
    std::deque<std::packaged_task<void()>> queue_;

    asio::Driver* drv_ = nullptr;
    std::string driverName_;
    long numOutputs_ = 0;
    ChannelChoice channels_;
    std::optional<asio::SampleType> driverType_;
    bool buffersCreated_ = false, started_ = false, outputReady_ = false;
    asio::BufferInfo buffers_[2] = {};
    long bufferSize_ = 0;
    size_t bufferFrames_ = 0; // device frames per callback: bytes per channel in DSD mode
    bool dsdMode_ = false;    // driver switched to DSD I/O format
    std::map<uint32_t, bool> dsdCaps_;
    static constexpr uint8_t kDsdSilence = 0x69;
    static constexpr uint32_t kTimedCallbacks = 64;
    uint32_t callbacks_ = 0; // driver thread only
    std::chrono::steady_clock::time_point firstCallback_;
    std::atomic<uint64_t> callbackPeriodUs_{0};
    DeviceFormat fmt_;
    std::vector<uint8_t> interleaved_;
    std::atomic<IRenderSource*> source_{nullptr};
    mutable std::mutex statusMutex_;
    OutputStatus status_;
};

} // namespace

std::unique_ptr<IOutput> createAsioOutput(const AsioSettings& s) {
    return std::make_unique<AsioOutput>(s);
}

bool showAsioControlPanel(IOutput* out) {
    auto a = dynamic_cast<AsioOutput*>(out);
    return a && a->controlPanel();
}

std::vector<DeviceEntry> listAsioDrivers() {
    std::vector<DeviceEntry> out;
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    // RegGetValueW guarantees a terminated REG_SZ result; anything else reads as empty.
    auto readString = [](HKEY key, const wchar_t* subKey, const wchar_t* value, wchar_t* buf, DWORD bytes) {
        if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) buf[0] = 0;
        return buf[0] != 0;
    };
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64], desc[256];
        if (!readString(root, name, L"CLSID", clsid, sizeof clsid)) continue;
        readString(root, name, L"Description", desc, sizeof desc);
        // Skip drivers whose DLL is gone (e.g. a stale Cayin RU6 entry, §15.3)
        std::wstring inproc = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
        wchar_t dll[MAX_PATH];
        bool present = readString(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr, dll, sizeof dll) &&
                       GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
        std::string label = toUtf8(desc[0] ? desc : name);
        if (!present) label += " (unavailable)";
        out.push_back({toUtf8(clsid), label});
    }
    RegCloseKey(root);
    return out;
}

} // namespace camel::audio
