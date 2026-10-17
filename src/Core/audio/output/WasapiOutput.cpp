#include "audio/output/WasapiOutput.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <atomic>
#include <cmath>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "audio/output/WaveFormat.h"
#include "util/Log.h"
#include "util/Text.h"

namespace camel::audio {
namespace {

constexpr REFERENCE_TIME kPollBuffer = 2'000'000; // 200 ms (docs/Plan.md §6 default)

wil::com_ptr<IMMDevice> findDevice(const std::string& id) {
    auto en = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_ALL);
    wil::com_ptr<IMMDevice> dev;
    HRESULT hr = id.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, &dev) : en->GetDevice(toWide(id).c_str(), &dev);
    if (FAILED(hr) || !dev) throw Error(ErrorCode::DeviceNotFound, "WASAPI device not found");
    DWORD state = 0;
    if (SUCCEEDED(dev->GetState(&state)) && state != DEVICE_STATE_ACTIVE)
        throw Error(ErrorCode::DeviceNotFound, "WASAPI device not active");
    return dev;
}

Error hrError(const char* what, HRESULT hr) {
    char buf[96];
    const int length = snprintf(buf, sizeof buf, "%s: 0x%08lX", what, (unsigned long)hr);
    if (length < 0 || size_t(length) >= sizeof buf) strcpy_s(buf, "WASAPI operation failed");
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE:
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return Error(ErrorCode::DeviceInUse, buf);
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return Error(ErrorCode::DeviceLost, buf);
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return Error(ErrorCode::FormatNotSupported, buf);
    default:
        return Error(ErrorCode::DriverError, buf);
    }
}

// Ensures COM (MTA) for the lifetime of a thread that may already have it.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

class WasapiOutput final : public IOutput {
public:
    explicit WasapiOutput(const WasapiSettings& s) : cfg_(s) {}
    ~WasapiOutput() override { close(); }

    bool supports(const DeviceFormat& f) override {
        std::lock_guard lock(capsMutex_);
        auto key = std::make_tuple(f.sampleRate, f.channels, int(f.type));
        if (auto it = caps_.find(key); it != caps_.end()) return it->second;
        ComScope com;
        bool ok = false;
        auto dev = findDevice(cfg_.deviceId);
        wil::com_ptr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
            WAVEFORMATEXTENSIBLE wf = makeWaveFormat(f);
            ok = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wf.Format, nullptr) == S_OK;
        }
        caps_[key] = ok;
        return ok;
    }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        close();
        fmt_ = f;
        source_ = source;
        quit_ = false;
        running_ = false;
        wake_.ResetEvent();
        std::promise<void> opened;
        auto result = opened.get_future();
        thread_ = std::thread([this, p = std::move(opened)]() mutable { threadMain(p); });
        try {
            result.get();
        } catch (...) {
            thread_.join();
            throw;
        }
    }

    void start() override { command(true); }
    void pause() override { command(false); }

    void close() override {
        if (!thread_.joinable()) return;
        quit_ = true;
        wake_.SetEvent();
        thread_.join();
    }

    OutputStatus status() const override {
        std::lock_guard lock(statusMutex_);
        return status_;
    }

private:
    void command(bool run) {
        if (!thread_.joinable()) return;
        running_ = run;
        wake_.SetEvent();
        applied_.wait(run); // returns once the render thread has applied the new state
    }

    void threadMain(std::promise<void>& opened) {
        ComScope com;
        wil::com_ptr<IAudioClient> client;
        wil::com_ptr<IAudioRenderClient> render;
        wil::unique_event bufferEvent(wil::EventOptions::None);
        UINT32 bufferFrames = 0;
        REFERENCE_TIME period = 0;
        try {
            auto dev = findDevice(cfg_.deviceId);
            THROW_IF_FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
            REFERENCE_TIME defP = 0, minP = 0;
            client->GetDevicePeriod(&defP, &minP);
            period = cfg_.period == WasapiPeriod::Minimum ? minP
                     : cfg_.period == WasapiPeriod::Custom
                         ? std::max<REFERENCE_TIME>(minP, REFERENCE_TIME(cfg_.customPeriodMs * 10000))
                         : defP;
            WAVEFORMATEXTENSIBLE wf = makeWaveFormat(fmt_);
            const DWORD flags = cfg_.eventDriven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
            const REFERENCE_TIME dur = cfg_.eventDriven ? period : std::max(kPollBuffer, period * 4);
            HRESULT hr =
                client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, dur, cfg_.eventDriven ? period : 0, &wf.Format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 frames = 0;
                client->GetBufferSize(&frames);
                period = REFERENCE_TIME(std::llround(10000.0 * 1000 * frames / fmt_.sampleRate));
                log::info("wasapi: buffer not aligned, retrying with %u frames", frames);
                client.reset();
                THROW_IF_FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, cfg_.eventDriven ? period : dur,
                                        cfg_.eventDriven ? period : 0, &wf.Format, nullptr);
            }
            if (FAILED(hr)) throw hrError("IAudioClient::Initialize", hr);
            if (cfg_.eventDriven) THROW_IF_FAILED(client->SetEventHandle(bufferEvent.get()));
            THROW_IF_FAILED(client->GetBufferSize(&bufferFrames));
            THROW_IF_FAILED(client->GetService(IID_PPV_ARGS(&render)));
            {
                std::lock_guard lock(statusMutex_);
                status_.mode = cfg_.eventDriven ? "WASAPI exclusive · event" : "WASAPI exclusive · polling";
                status_.bufferFrames = bufferFrames;
                char d[64];
                const int length = snprintf(d, sizeof d, "period %.2f ms", static_cast<double>(period) / 10000.0);
                status_.detail = length < 0 || size_t(length) >= sizeof d ? "WASAPI period unavailable" : d;
            }
            opened.set_value();
        } catch (const Error&) {
            opened.set_exception(std::current_exception());
            return;
        } catch (const wil::ResultException& e) {
            opened.set_exception(std::make_exception_ptr(hrError("WASAPI open", e.GetErrorCode())));
            return;
        }

        DWORD task = 0;
        HANDLE av = AvSetMmThreadCharacteristicsW(kMmcssTask, &task);
        const UINT32 bpf = fmt_.bytesPerFrame();
        bool started = false, primed = false;
        int timeouts = 0;
        auto fill = [&](UINT32 frames) -> HRESULT {
            BYTE* p = nullptr;
            HRESULT hr = render->GetBuffer(frames, &p);
            if (FAILED(hr)) return hr;
            size_t got = source_->render(p, frames);
            if (got < frames) memset(p + got * bpf, 0, size_t(frames - got) * bpf);
            return render->ReleaseBuffer(frames, 0);
        };
        HRESULT err = S_OK;
        while (!quit_) {
            bool want = running_;
            if (want != started) {
                if (want) {
                    if (!primed) { // exclusive mode requires a full buffer before Start
                        err = fill(bufferFrames);
                        primed = true;
                    }
                    if (SUCCEEDED(err)) err = client->Start();
                } else {
                    err = client->Stop();
                }
                started = want;
                applied_.notify(want);
                if (FAILED(err)) break;
            }
            if (!started) {
                WaitForSingleObject(wake_.get(), INFINITE);
                continue;
            }
            if (cfg_.eventDriven) {
                HANDLE hs[2] = {bufferEvent.get(), wake_.get()};
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
                if (w == WAIT_OBJECT_0 + 1) continue;
                if (w == WAIT_TIMEOUT) {
                    if (++timeouts >= 2) {
                        err = AUDCLNT_E_DEVICE_INVALIDATED;
                        break;
                    }
                    continue;
                }
                timeouts = 0;
                err = fill(bufferFrames);
                if (FAILED(err)) break;
            } else {
                WaitForSingleObject(wake_.get(), DWORD(std::max<REFERENCE_TIME>(1, period / 20000)));
                UINT32 pad = 0;
                err = client->GetCurrentPadding(&pad);
                if (FAILED(err)) break;
                if (bufferFrames > pad) {
                    err = fill(bufferFrames - pad);
                    if (FAILED(err)) break;
                }
            }
        }
        if (started) client->Stop();
        if (av) AvRevertMmThreadCharacteristics(av);
        if (FAILED(err)) {
            log::error("wasapi: stream failed 0x%08lX", (unsigned long)err);
            applied_.notify(running_);
            raiseError(hrError("WASAPI stream", err));
        }
    }

    // Lets start()/pause() wait until the render thread applied the change (or quit).
    class Applied {
    public:
        void notify(bool state) {
            std::lock_guard lock(m_);
            state_ = state;
            ++gen_;
            cv_.notify_all();
        }
        void wait(bool state) {
            std::unique_lock lock(m_);
            cv_.wait_for(lock, std::chrono::seconds(3), [&] { return state_ == state; });
        }

    private:
        std::mutex m_;
        std::condition_variable cv_;
        bool state_ = false;
        uint64_t gen_ = 0;
    };

    WasapiSettings cfg_;
    DeviceFormat fmt_;
    IRenderSource* source_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
    wil::unique_event wake_{wil::EventOptions::None};
    Applied applied_;
    mutable std::mutex statusMutex_;
    OutputStatus status_;
    std::mutex capsMutex_;
    std::map<std::tuple<uint32_t, uint32_t, int>, bool> caps_;
};

} // namespace

std::unique_ptr<IOutput> createWasapiOutput(const WasapiSettings& s) {
    return std::make_unique<WasapiOutput>(s);
}

std::vector<DeviceEntry> listWasapiDevices() {
    ComScope com;
    std::vector<DeviceEntry> out;
    try {
        auto en = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_ALL);
        wil::com_ptr<IMMDeviceCollection> col;
        THROW_IF_FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col));
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            wil::com_ptr<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            wil::unique_cotaskmem_string id;
            d->GetId(&id);
            wil::com_ptr<IPropertyStore> ps;
            std::string name = "?";
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                wil::unique_prop_variant v;
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = toUtf8(v.pwszVal);
            }
            out.push_back({toUtf8(id.get()), name});
        }
    } catch (const wil::ResultException& e) {
        log::error("wasapi: enumerate failed 0x%08lX", (unsigned long)e.GetErrorCode());
    }
    return out;
}

} // namespace camel::audio
