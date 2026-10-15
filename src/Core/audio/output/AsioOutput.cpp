#include "audio/output/AsioOutput.h"

#include <windows.h>
#include <objbase.h>

#pragma warning(push, 0)
#include <iasiodrv.h>  // Steinberg ASIO SDK 2.3.4 (third_party/asiosdk, GPLv3)
#pragma warning(pop)

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "util/Log.h"
#include "util/Text.h"

namespace camel::audio {
namespace {

enum : long { kStInt16LSB = 16, kStInt24LSB = 17, kStInt32LSB = 18 };

std::optional<SampleType> mapSampleType(ASIOSampleType t) {
    switch (t) {
    case kStInt16LSB: return SampleType::Int16;
    case kStInt24LSB: return SampleType::Int24;
    case kStInt32LSB: return SampleType::Int32;
    default: return std::nullopt;  // float and LSB-justified 32-bit types are not bit-perfect targets in the MVP
    }
}

const char* asioTypeName(ASIOSampleType t) {
    switch (t) {
    case 16: return "Int16LSB";
    case 17: return "Int24LSB";
    case 18: return "Int32LSB";
    case 19: return "Float32LSB";
    case 20: return "Float64LSB";
    case 24: return "Int32LSB16";
    case 25: return "Int32LSB18";
    case 26: return "Int32LSB20";
    case 27: return "Int32LSB24";
    default: return "other";
    }
}

class AsioOutput;
AsioOutput* g_active = nullptr;  // the SDK callbacks carry no user pointer (§5.6)

class AsioOutput final : public IOutput {
public:
    explicit AsioOutput(const AsioSettings& s) : cfg_(s) {
        thread_ = std::thread([this] { staMain(); });
        std::unique_lock lock(qMutex_);
        qCv_.wait(lock, [&] { return hwnd_ != nullptr || staFailed_; });
    }

    ~AsioOutput() override {
        close();
        invoke([this] { unload(); });
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
                if (driverType_ && mapSampleType(*driverType_) == f.type && f.channels == 2)
                    ok = drv_->canSampleRate(ASIOSampleRate(f.sampleRate)) == ASE_OK;
            });
        } catch (const Error&) {
            return true;  // let open() report why the driver is unusable
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
            source_.store(nullptr);
            if (g_active == this) g_active = nullptr;
        });
    }

    OutputStatus status() const override {
        std::lock_guard lock(statusMutex_);
        return status_;
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
        const size_t frames = size_t(bufferSize_);
        const uint32_t bps = bytesPerSample(fmt_.type);
        size_t got = src ? src->render(interleaved_.data(), frames) : 0;
        if (got < frames) memset(interleaved_.data() + got * 2 * bps, 0, (frames - got) * 2 * bps);
        auto* l = static_cast<uint8_t*>(buffers_[0].buffers[index]);
        auto* r = static_cast<uint8_t*>(buffers_[1].buffers[index]);
        const uint8_t* in = interleaved_.data();
        for (size_t i = 0; i < frames; i++) {
            memcpy(l + i * bps, in, bps);
            memcpy(r + i * bps, in + bps, bps);
            in += 2 * bps;
        }
        if (outputReady_) drv_->outputReady();
    }
    long onMessage(long selector, long value) noexcept {
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                   value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioBufferSizeChange;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
            log::info("asio: driver requested reset (%ld)", selector);
            raiseRestart();
            return 1;
        case kAsioResyncRequest:
        case kAsioLatenciesChanged: return 1;
        case kAsioSupportsTimeInfo: return 1;
        default: return 0;
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
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"CamelPlayer ASIO", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
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
            snprintf(msg, sizeof msg, "cannot load ASIO driver (0x%08lX)", (unsigned long)hr);
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
    }

    void openOnSta(const DeviceFormat& f, IRenderSource* source) {
        load();
        if (cfg_.outLeft >= numOutputs_ || cfg_.outRight >= numOutputs_) throw Error(ErrorCode::DeviceNotFound, "ASIO output channel out of range");
        if (!driverType_ || mapSampleType(*driverType_) != f.type) throw Error(ErrorCode::FormatNotSupported, "ASIO sample type mismatch");
        ASIOSampleRate cur = 0;
        drv_->getSampleRate(&cur);
        if (cur != ASIOSampleRate(f.sampleRate) && drv_->setSampleRate(ASIOSampleRate(f.sampleRate)) != ASE_OK)
            throw Error(ErrorCode::FormatNotSupported, "ASIOSetSampleRate failed");
        long minS = 0, maxS = 0, pref = 0, gran = 0;
        drv_->getBufferSize(&minS, &maxS, &pref, &gran);
        long size = pref;
        if (cfg_.bufferSize > 0 && minS != maxS) {
            size = std::clamp(cfg_.bufferSize, minS, maxS);
            if (gran > 0) size = minS + (size - minS) / gran * gran;
            else if (gran == -1) {  // powers of two
                long p = minS;
                while (p * 2 <= size) p *= 2;
                size = p;
            }
        }
        buffers_[0] = {ASIOFalse, cfg_.outLeft, {nullptr, nullptr}};
        buffers_[1] = {ASIOFalse, cfg_.outRight, {nullptr, nullptr}};
        static ASIOCallbacks callbacks = {
            [](long index, ASIOBool) {
                if (g_active) g_active->onBufferSwitch(index);
            },
            [](ASIOSampleRate) {
                if (g_active) g_active->onRateChanged();
            },
            [](long selector, long value, void*, double*) -> long { return g_active ? g_active->onMessage(selector, value) : 0; },
            [](ASIOTime* t, long index, ASIOBool) -> ASIOTime* {
                if (g_active) g_active->onBufferSwitch(index);
                return t;
            },
        };
        fmt_ = f;
        bufferSize_ = size;
        interleaved_.assign(size_t(size) * f.bytesPerFrame(), 0);
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
        status_.mode = "ASIO · " + driverName_;
        status_.bufferFrames = uint32_t(size);
        char d[160];
        snprintf(d, sizeof d, "%s, buffer %ld (min %ld / max %ld), output latency %ld%s", asioTypeName(*driverType_), size, minS, maxS,
                 outLat, outLat == 0 ? " (driver reports 0)" : "");
        status_.detail = d;
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
    DeviceFormat fmt_;
    std::vector<uint8_t> interleaved_;
    std::atomic<IRenderSource*> source_{nullptr};
    mutable std::mutex statusMutex_;
    OutputStatus status_;
};

}  // namespace

std::unique_ptr<IOutput> createAsioOutput(const AsioSettings& s) { return std::make_unique<AsioOutput>(s); }

bool showAsioControlPanel(IOutput* out) {
    auto a = dynamic_cast<AsioOutput*>(out);
    return a && a->controlPanel();
}

std::vector<DeviceEntry> listAsioDrivers() {
    std::vector<DeviceEntry> out;
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t clsid[64] = {}, desc[256] = {};
        DWORD sz = sizeof clsid;
        RegQueryValueExW(k, L"CLSID", nullptr, nullptr, reinterpret_cast<BYTE*>(clsid), &sz);
        sz = sizeof desc;
        RegQueryValueExW(k, L"Description", nullptr, nullptr, reinterpret_cast<BYTE*>(desc), &sz);
        RegCloseKey(k);
        if (!clsid[0]) continue;
        // Skip drivers whose DLL is gone (e.g. a stale Cayin RU6 entry, §15.3)
        std::wstring inproc = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
        wchar_t dll[MAX_PATH] = {};
        LONG dllLen = sizeof dll;
        bool present = RegQueryValueW(HKEY_CLASSES_ROOT, inproc.c_str(), dll, &dllLen) == ERROR_SUCCESS &&
                       GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
        std::string label = toUtf8(desc[0] ? desc : name);
        if (!present) label += " (unavailable)";
        out.push_back({toUtf8(clsid), label});
    }
    RegCloseKey(root);
    return out;
}

}  // namespace camel::audio
