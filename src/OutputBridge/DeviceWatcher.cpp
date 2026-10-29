// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "OutputBridge/DeviceWatcher.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <wil/com.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "Common/AudioTypes.h"

namespace camel::audio {
namespace {

enum class Watch : uint8_t { DefaultRender, Devices };

class Client final : public IMMNotificationClient {
public:
    Client(Watch watch, std::function<void()> onChange) : watch_(watch), onChange_(std::move(onChange)) {}

    void stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (watch_ == Watch::DefaultRender && flow == eRender && role == eConsole) notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return deviceChanged(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT deviceChanged() {
        if (watch_ == Watch::Devices) notify();
        return S_OK;
    }
    void notify() {
        std::lock_guard lock(mutex_);
        if (!stopped_) onChange_();
    }

    Watch watch_;
    std::function<void()> onChange_;
    std::mutex mutex_;
    bool stopped_ = false;
    std::atomic<ULONG> refs_{1};
};

class EndpointWatcher final : public DeviceWatcher {
public:
    EndpointWatcher(Watch watch, std::function<void()> onChange) {
        enumerator_ = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_ALL);
        client_ = new Client(watch, std::move(onChange));
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(client_))) {
            client_->Release();
            throw Error(ErrorCode::DriverError, "cannot watch the audio endpoints");
        }
    }
    ~EndpointWatcher() override {
        client_->stop();
        enumerator_->UnregisterEndpointNotificationCallback(client_);
        client_->Release();
    }
    EndpointWatcher(const EndpointWatcher&) = delete;
    EndpointWatcher& operator=(const EndpointWatcher&) = delete;

private:
    wil::com_ptr<IMMDeviceEnumerator> enumerator_;
    Client* client_ = nullptr;
};

} // namespace

std::unique_ptr<DeviceWatcher> watchDefaultRenderDevice(std::function<void()> onDefaultChanged) {
    try {
        return std::make_unique<EndpointWatcher>(Watch::DefaultRender, std::move(onDefaultChanged));
    } catch (const wil::ResultException&) {
        throw Error(ErrorCode::DriverError, "cannot watch the Windows default output device");
    }
}

std::unique_ptr<DeviceWatcher> watchAudioDevices(std::function<void()> onChange) {
    try {
        return std::make_unique<EndpointWatcher>(Watch::Devices, std::move(onChange));
    } catch (const wil::ResultException&) {
        throw Error(ErrorCode::DriverError, "cannot watch the audio devices");
    }
}

} // namespace camel::audio
