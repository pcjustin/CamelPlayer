#include "OutputBridge/AsioOutput.h"

#include <windows.h>
#include <objbase.h>

#pragma warning(push, 0)
#include <iasiodrv.h> // Steinberg ASIO SDK 2.3.4 (third_party/asiosdk, GPLv3)
#pragma warning(pop)

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
#include "OutputBridge/AsioLayout.h"

namespace camel::audio {
namespace {

constexpr long kStInt16LSB = 16;
constexpr long kStInt24LSB = 17;
constexpr long kStInt32LSB = 18;
constexpr long kStDsdInt8LSB1 = 32;
constexpr long kStDsdInt8MSB1 = 33;

std::optional<SampleType> mapSampleType(ASIOSampleType t) {
    switch (t) {
    case kStInt16LSB:
        return SampleType::Int16;
    case kStInt24LSB:
        return SampleType::Int24;
    case kStInt32LSB:
        return SampleType::Int32;
    default:
        return std::nullopt; // float and LSB-justified 32-bit types are not bit-perfect targets in the MVP
    }
}

const char* asioTypeName(ASIOSampleType t) {
    switch (t) {
    case 16:
        return "Int16LSB";
    case 17:
        return "Int24LSB";
    case 18:
        return "Int32LSB";
    case 19:
        return "Float32LSB";
    case 20:
        return "Float64LSB";
    case 24:
        return "Int32LSB16";
    case 25:
        return "Int32LSB18";
    case 26:
        return "Int32LSB20";
    case 27:
        return "Int32LSB24";
    case 32:
        return "DSDInt8LSB1";
    case 33:
        return "DSDInt8MSB1";
    case 40:
        return "DSDInt8NER8";
    default:
        return "other";
    }
}

class AsioOutput;
std::atomic<AsioOutput*> g_active{nullptr}; // the SDK callbacks carry no user pointer (§5.6)

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
                if (cfg_.outLeft >= numOutputs_ || cfg_.outRight >= numOutputs_) {
                    ok = true; // open() reports the out-of-range channel
                    return;
                }
                if (f.type == SampleType::Dsd8)
                    ok = f.channels == 2 && supportsDsd(f.sampleRate);
                else if (driverType_ && mapSampleType(*driverType_) == f.type && f.channels == 2)
                    ok = drv_->canSampleRate(ASIOSampleRate(f.sampleRate)) == ASE_OK;
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
                if (drv_->start() != ASE_OK) throw Error(ErrorCode::DriverError, "ASIOStart failed");
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
            if (dsdMode_) setIoFormat(kASIOPCMFormat); // leave the driver in PCM mode for the next stream
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
            ok = drv_->controlPanel() == ASE_OK;
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
        splitStereo(interleaved_.data(), frames, bps, dsdMode_ && *driverType_ == kStDsdInt8LSB1,
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
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                   value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioBufferSizeChange;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
            log::info("asio: driver requested reset (%ld)", selector);
            raiseRestart();
            return 1;
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioSupportsTimeInfo:
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
        if (id.empty()) {
            auto drivers = listAsioDrivers();
            if (drivers.empty()) throw Error(ErrorCode::DeviceNotFound, "no ASIO driver installed");
            id = drivers.front().id;
        }
        if (FAILED(CLSIDFromString(toWide(id).c_str(), &clsid))) throw Error(ErrorCode::DeviceNotFound, "bad ASIO driver id");
        IASIO* d = nullptr;
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
            throw Error(ErrorCode::DriverError, std::string("ASIO init failed: ") + err);
        }
        drv_ = d;
        char name[64] = {};
        drv_->getDriverName(name);
        driverName_ = name;
        long ins = 0, outs = 0;
        drv_->getChannels(&ins, &outs);
        numOutputs_ = outs;
        ASIOChannelInfo ci{};
        ci.channel = cfg_.outLeft;
        ci.isInput = ASIOFalse;
        if (cfg_.outLeft < outs && drv_->getChannelInfo(&ci) == ASE_OK) driverType_ = ci.type;
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

    bool setIoFormat(ASIOIoFormatType type) {
        ASIOIoFormat format{};
        format.FormatType = type;
        const bool ok = drv_->future(kAsioSetIoFormat, &format) == ASE_SUCCESS;
        if (ok) dsdMode_ = type == kASIODSDFormat;
        return ok;
    }

    std::optional<ASIOSampleType> outputType() {
        ASIOChannelInfo ci{};
        ci.channel = cfg_.outLeft;
        ci.isInput = ASIOFalse;
        if (cfg_.outLeft < numOutputs_ && drv_->getChannelInfo(&ci) == ASE_OK) return ci.type;
        return std::nullopt;
    }

    // Native DSD at `byteFrameRate` * 8 samples per second, with a bit-per-sample layout the Engine can fill. Probing
    // switches the driver to DSD mode and back, so an open stream answers only for its own format.
    bool supportsDsd(uint32_t byteFrameRate) {
        if (const auto it = dsdCaps_.find(byteFrameRate); it != dsdCaps_.end()) return it->second;
        if (buffersCreated_) return fmt_ == DeviceFormat{byteFrameRate, 2, SampleType::Dsd8};
        ASIOIoFormat query{};
        query.FormatType = kASIODSDFormat;
        bool ok = drv_->future(kAsioCanDoIoFormat, &query) == ASE_SUCCESS && setIoFormat(kASIODSDFormat);
        if (ok) {
            const auto type = outputType();
            ok = (type == kStDsdInt8MSB1 || type == kStDsdInt8LSB1) &&
                 drv_->canSampleRate(ASIOSampleRate(double(byteFrameRate) * 8)) == ASE_OK;
            setIoFormat(kASIOPCMFormat);
        }
        dsdCaps_[byteFrameRate] = ok;
        return ok;
    }

    void openOnSta(const DeviceFormat& f, IRenderSource* source) {
        load();
        if (cfg_.outLeft >= numOutputs_ || cfg_.outRight >= numOutputs_)
            throw Error(ErrorCode::DeviceNotFound, "ASIO output channel out of range");
        const bool dsd = f.type == SampleType::Dsd8;
        if (dsd != dsdMode_ && !setIoFormat(dsd ? kASIODSDFormat : kASIOPCMFormat))
            throw Error(ErrorCode::FormatNotSupported, dsd ? "ASIO driver refused DSD mode" : "ASIO driver refused PCM mode");
        driverType_ = outputType();
        const bool typeOk = dsd ? driverType_ == kStDsdInt8MSB1 || driverType_ == kStDsdInt8LSB1
                                : driverType_ && mapSampleType(*driverType_) == f.type;
        if (!typeOk) throw Error(ErrorCode::FormatNotSupported, "ASIO sample type mismatch");
        // DSD rates are set in DSD samples per second (2.8224 MHz for DSD64); a device frame holds eight of them.
        const ASIOSampleRate rate = dsd ? double(f.sampleRate) * 8 : double(f.sampleRate);
        ASIOSampleRate cur = 0;
        drv_->getSampleRate(&cur);
        if (cur != rate && drv_->setSampleRate(rate) != ASE_OK)
            throw Error(ErrorCode::FormatNotSupported, "ASIOSetSampleRate failed");
        long minS = 0, maxS = 0, pref = 0, gran = 0;
        drv_->getBufferSize(&minS, &maxS, &pref, &gran);
        long size = pref;
        if (cfg_.bufferSize > 0 && minS != maxS) {
            size = std::clamp(cfg_.bufferSize, minS, maxS);
            if (gran > 0)
                size = minS + (size - minS) / gran * gran;
            else if (gran == -1) { // powers of two
                long p = minS;
                while (p * 2 <= size) p *= 2;
                size = p;
            }
        }
        buffers_[0] = {ASIOFalse, cfg_.outLeft, {nullptr, nullptr}};
        buffers_[1] = {ASIOFalse, cfg_.outRight, {nullptr, nullptr}};
        static ASIOCallbacks callbacks = {
            [](long index, ASIOBool) {
                if (auto* active = g_active.load(std::memory_order_acquire)) active->onBufferSwitch(index);
            },
            [](ASIOSampleRate) {
                if (auto* active = g_active.load(std::memory_order_acquire)) active->onRateChanged();
            },
            [](long selector, long value, void*, double*) -> long {
                auto* active = g_active.load(std::memory_order_acquire);
                return active ? active->onMessage(selector, value) : 0;
            },
            [](ASIOTime* t, long index, ASIOBool) -> ASIOTime* {
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
        if (drv_->createBuffers(buffers_, 2, size, &callbacks) != ASE_OK) {
            g_active = nullptr;
            throw Error(ErrorCode::DriverError, "ASIOCreateBuffers failed");
        }
        buffersCreated_ = true;
        outputReady_ = drv_->outputReady() == ASE_OK;
        source_.store(source, std::memory_order_release);
        long inLat = 0, outLat = 0;
        drv_->getLatencies(&inLat, &outLat);
        std::lock_guard lock(statusMutex_);
        status_.mode = "ASIO · " + driverName_ + (dsd ? " · native DSD" : "");
        status_.bufferFrames = uint32_t(bufferFrames_);
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

    IASIO* drv_ = nullptr;
    std::string driverName_;
    long numOutputs_ = 0;
    std::optional<ASIOSampleType> driverType_;
    bool buffersCreated_ = false, started_ = false, outputReady_ = false;
    ASIOBufferInfo buffers_[2] = {};
    long bufferSize_ = 0;
    size_t bufferFrames_ = 0; // device frames per callback: bytes per channel in DSD mode
    bool dsdMode_ = false;    // driver switched to kASIODSDFormat
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
