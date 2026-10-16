#include "audio/output/KsOutput.h"

#include "audio/output/WaveFormat.h" // windows + ks headers first

#include <avrt.h>
#include <setupapi.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "util/Log.h"
#include "util/Text.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

namespace camel::audio {
namespace {

// Not in the user-mode SDK headers (WDK only).
const GUID kRtAudioSet = {0xa855a48c, 0x2f78, 0x4729, {0x90, 0x51, 0x19, 0x68, 0x74, 0x6b, 0x9e, 0xef}};

typedef DWORD(WINAPI* KsCreatePinFn)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);

KsCreatePinFn ksCreatePinFn() {
    static KsCreatePinFn fn = [] {
        HMODULE m = LoadLibraryW(L"ksuser.dll");
        return m ? reinterpret_cast<KsCreatePinFn>(GetProcAddress(m, "KsCreatePin")) : nullptr;
    }();
    return fn;
}

bool ksIoctl(HANDLE h, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize, DWORD* ret = nullptr) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, code, in, inSize, out, outSize, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &got, TRUE);
    DWORD e = ok ? 0 : GetLastError();
    CloseHandle(ov.hEvent);
    if (ret) *ret = got;
    SetLastError(e);
    return ok != FALSE;
}

KSPROPERTY ksProp(const GUID& set, ULONG id, ULONG flags) {
    KSPROPERTY p{};
    p.Set = set;
    p.Id = id;
    p.Flags = flags;
    return p;
}

std::vector<BYTE> pinPropVar(HANDLE f, ULONG pin, ULONG id) {
    KSP_PIN p{};
    p.Property = ksProp(KSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET);
    p.PinId = pin;
    DWORD need = 0;
    ksIoctl(f, IOCTL_KS_PROPERTY, &p, sizeof p, nullptr, 0, &need);
    if (!need) return {};
    std::vector<BYTE> b(need);
    if (!ksIoctl(f, IOCTL_KS_PROPERTY, &p, sizeof p, b.data(), need, &need)) return {};
    b.resize(need);
    return b;
}

template <class T> bool pinProp(HANDLE f, ULONG pin, ULONG id, T& out) {
    KSP_PIN p{};
    p.Property = ksProp(KSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET);
    p.PinId = pin;
    return ksIoctl(f, IOCTL_KS_PROPERTY, &p, sizeof p, &out, sizeof out);
}

struct Filter {
    std::wstring path;
    std::string name;
    bool realtime = false;
};

std::string regString(HKEY k, const wchar_t* value) {
    wchar_t buf[512];
    DWORD sz = sizeof buf, type = 0;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) != ERROR_SUCCESS || type != REG_SZ)
        return {};
    buf[511] = 0;
    return toUtf8(buf);
}

std::vector<Filter> renderFilters() {
    std::vector<Filter> out;
    HDEVINFO di = SetupDiGetClassDevsW(&KSCATEGORY_RENDER, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
        if (!SetupDiEnumDeviceInterfaces(di, nullptr, &KSCATEGORY_RENDER, i, &ifd)) break;
        SP_DEVICE_INTERFACE_DATA alias{sizeof alias};
        if (!SetupDiGetDeviceInterfaceAlias(di, &ifd, &KSCATEGORY_AUDIO, &alias)) continue;
        SP_DEVICE_INTERFACE_DATA rt{sizeof rt};
        bool realtime = SetupDiGetDeviceInterfaceAlias(di, &ifd, &KSCATEGORY_REALTIME, &rt) != FALSE;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> b(need);
        auto det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(b.data());
        det->cbSize = sizeof(*det);
        SP_DEVINFO_DATA dev{sizeof dev};
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dev)) continue;
        std::string ifName;
        HKEY k = SetupDiOpenDeviceInterfaceRegKey(di, &ifd, 0, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            ifName = regString(k, L"FriendlyName");
            RegCloseKey(k);
        }
        wchar_t devName[512] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dev, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(devName),
                                               sizeof devName, nullptr))
            SetupDiGetDeviceRegistryPropertyW(di, &dev, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(devName),
                                              sizeof devName, nullptr);
        std::string name = toUtf8(devName);
        if (!ifName.empty()) name += " / " + ifName;
        out.push_back({det->DevicePath, name, realtime});
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

HANDLE openFilter(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
}

struct PinInfo {
    bool pcmRender = false, standard = false, looped = false;
    std::string name;
    struct Range {
        bool isFloat;
        ULONG minBits, maxBits, minRate, maxRate;
    };
    std::vector<Range> ranges;
};

PinInfo pinInfo(HANDLE f, ULONG pin) {
    PinInfo pi;
    KSPIN_DATAFLOW flow = KSPIN_DATAFLOW_IN;
    KSPIN_COMMUNICATION comm{};
    if (!pinProp(f, pin, KSPROPERTY_PIN_DATAFLOW, flow) || !pinProp(f, pin, KSPROPERTY_PIN_COMMUNICATION, comm)) return pi;
    if (flow != KSPIN_DATAFLOW_IN || !(comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH)) return pi;
    auto nm = pinPropVar(f, pin, KSPROPERTY_PIN_NAME);
    if (!nm.empty()) {
        nm.push_back(0);
        nm.push_back(0);
        pi.name = toUtf8(reinterpret_cast<const wchar_t*>(nm.data()));
    }
    auto itf = pinPropVar(f, pin, KSPROPERTY_PIN_INTERFACES);
    if (itf.size() >= sizeof(KSMULTIPLE_ITEM)) {
        auto mi = reinterpret_cast<KSMULTIPLE_ITEM*>(itf.data());
        auto ids = reinterpret_cast<KSIDENTIFIER*>(mi + 1);
        for (ULONG i = 0; i < mi->Count && reinterpret_cast<BYTE*>(ids + i + 1) <= itf.data() + itf.size(); i++) {
            if (ids[i].Set != KSINTERFACESETID_Standard) continue;
            if (ids[i].Id == KSINTERFACE_STANDARD_STREAMING) pi.standard = true;
            if (ids[i].Id == KSINTERFACE_STANDARD_LOOPED_STREAMING) pi.looped = true;
        }
    }
    auto dr = pinPropVar(f, pin, KSPROPERTY_PIN_DATARANGES);
    if (dr.size() < sizeof(KSMULTIPLE_ITEM)) return pi;
    auto mi = reinterpret_cast<KSMULTIPLE_ITEM*>(dr.data());
    BYTE* p = reinterpret_cast<BYTE*>(mi + 1);
    BYTE* end = dr.data() + dr.size();
    bool attrNext = false;
    for (ULONG i = 0; i < mi->Count && p + sizeof(KSDATARANGE) <= end; i++) {
        auto r = reinterpret_cast<KSDATARANGE*>(p);
        ULONG sz = r->FormatSize;
        if (!sz) break;
        if (attrNext) { // attribute list following a range with KSDATARANGE_ATTRIBUTES
            attrNext = false;
            p += (sz + 7) & ~7u;
            continue;
        }
        bool audio = r->MajorFormat == KSDATAFORMAT_TYPE_AUDIO || r->MajorFormat == KSDATAFORMAT_TYPE_WILDCARD;
        bool isFloat = r->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        bool pcm = r->SubFormat == KSDATAFORMAT_SUBTYPE_PCM || isFloat || r->SubFormat == KSDATAFORMAT_SUBTYPE_WILDCARD;
        bool wfx = r->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX || r->Specifier == KSDATAFORMAT_SPECIFIER_WILDCARD;
        if (audio && pcm && wfx && sz >= sizeof(KSDATARANGE_AUDIO)) {
            auto a = reinterpret_cast<KSDATARANGE_AUDIO*>(r);
            pi.ranges.push_back({isFloat, a->MinimumBitsPerSample, a->MaximumBitsPerSample, a->MinimumSampleFrequency,
                                 a->MaximumSampleFrequency});
            pi.pcmRender = true;
        }
        if (r->Flags & KSDATARANGE_ATTRIBUTES) attrNext = true;
        p += (sz + 7) & ~7u;
    }
    return pi;
}

DWORD createPin(HANDLE filter, ULONG pinId, bool looped, const DeviceFormat& fmt, HANDLE* pin) {
    BYTE buf[sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)] = {};
    auto c = reinterpret_cast<KSPIN_CONNECT*>(buf);
    c->Interface.Set = KSINTERFACESETID_Standard;
    c->Interface.Id = looped ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;
    c->Medium.Set = KSMEDIUMSETID_Standard;
    c->Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    c->PinId = pinId;
    c->Priority.PriorityClass = KSPRIORITY_NORMAL;
    c->Priority.PrioritySubClass = 1;
    auto f = reinterpret_cast<KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(c + 1);
    WAVEFORMATEXTENSIBLE wf = makeWaveFormat(fmt);
    f->DataFormat.FormatSize = sizeof(*f);
    f->DataFormat.SampleSize = wf.Format.nBlockAlign;
    f->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    f->DataFormat.SubFormat = wf.SubFormat;
    f->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    f->WaveFormatExt = wf;
    *pin = nullptr;
    KsCreatePinFn fn = ksCreatePinFn();
    return fn ? fn(filter, c, GENERIC_READ | GENERIC_WRITE, pin) : ERROR_PROC_NOT_FOUND;
}

bool setState(HANDLE pin, KSSTATE s) {
    KSPROPERTY p = ksProp(KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET);
    return ksIoctl(pin, IOCTL_KS_PROPERTY, &p, sizeof p, &s, sizeof s);
}

bool parseDeviceId(const std::string& id, std::wstring& path, ULONG& pin) {
    auto bar = id.rfind('|');
    if (bar == std::string::npos) return false;
    path = toWide(id.substr(0, bar));
    pin = ULONG(std::stoul(id.substr(bar + 1)));
    return true;
}

enum class Mode : uint8_t { RtEvent, RtPoll, Standard };

class KsOutput final : public IOutput {
public:
    explicit KsOutput(const KsSettings& s) : cfg_(s) {
        if (!parseDeviceId(cfg_.deviceId, path_, pinId_)) { // default: first PCM render pin
            auto pins = listKsPins();
            if (!pins.empty()) parseDeviceId(pins.front().id, path_, pinId_);
        }
        for (auto& f : renderFilters())
            if (_wcsicmp(f.path.c_str(), path_.c_str()) == 0) realtimeFilter_ = f.realtime;
    }
    ~KsOutput() override { close(); }

    bool supports(const DeviceFormat& f) override {
        std::lock_guard lock(capsMutex_);
        auto key = std::make_tuple(f.sampleRate, f.channels, int(f.type));
        if (auto it = caps_.find(key); it != caps_.end()) return it->second;
        if (path_.empty()) return false;
        HANDLE filter = openFilter(path_);
        if (filter == INVALID_HANDLE_VALUE) return false;
        PinInfo pi = pinInfo(filter, pinId_);
        bool ok = false;
        bool definite = true;
        if (pi.pcmRender) {
            HANDLE pin = nullptr;
            DWORD e = createPin(filter, pinId_, useLooped(pi), f, &pin);
            if (e == ERROR_SUCCESS && pin) {
                ok = true;
                CloseHandle(pin);
            } else if (e != ERROR_NO_MATCH) {
                // Busy pins fail with driver-specific codes (§15.5). Answer from the data ranges so that
                // open() runs and reports "in use" instead of the track being skipped as unsupported.
                definite = false;
                const ULONG bits = bytesPerSample(f.type) * 8;
                for (auto& r : pi.ranges)
                    if (r.isFloat == (f.type == SampleType::Float32) && bits >= r.minBits && bits <= r.maxBits &&
                        f.sampleRate >= r.minRate && f.sampleRate <= r.maxRate)
                        ok = true;
            }
        }
        CloseHandle(filter);
        if (definite) caps_[key] = ok;
        return ok;
    }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        close();
        fmt_ = f;
        source_ = source;
        quit_ = false;
        running_ = false;
        ResetEvent(wake_);
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
        SetEvent(wake_);
        thread_.join();
    }

    OutputStatus status() const override {
        std::lock_guard lock(statusMutex_);
        return status_;
    }

private:
    bool useLooped(const PinInfo& pi) const {
        switch (cfg_.mode) {
        case KsMode::WaveRtEvent:
        case KsMode::WaveRtPoll:
            return true;
        case KsMode::Standard:
            return false;
        case KsMode::Auto:
            return pi.looped && realtimeFilter_;
        }
        return false;
    }

    void command(bool run) {
        if (!thread_.joinable()) return;
        std::unique_lock lock(cmdMutex_);
        running_ = run;
        SetEvent(wake_);
        cmdCv_.wait_for(lock, std::chrono::seconds(3), [&] { return applied_ == run || threadDone_; });
    }

    void notifyApplied(bool state) {
        std::lock_guard lock(cmdMutex_);
        applied_ = state;
        cmdCv_.notify_all();
    }

    void setStatus(const char* mode, uint32_t frames, const std::string& detail) {
        std::lock_guard lock(statusMutex_);
        status_.mode = mode;
        status_.bufferFrames = frames;
        status_.detail = detail;
    }

    void threadMain(std::promise<void>& opened);
    void runWaveRt(HANDLE pin, bool notify, std::promise<void>& opened);
    void runStandard(HANDLE pin, std::promise<void>& opened);
    bool waitRunning(); // blocks while paused; false when quitting
    void finish(DWORD err, const char* what);

    KsSettings cfg_;
    std::wstring path_;
    ULONG pinId_ = 0;
    bool realtimeFilter_ = false;
    DeviceFormat fmt_;
    IRenderSource* source_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
    HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex cmdMutex_;
    std::condition_variable cmdCv_;
    bool applied_ = false, threadDone_ = false;
    mutable std::mutex statusMutex_;
    OutputStatus status_;
    std::mutex capsMutex_;
    std::map<std::tuple<uint32_t, uint32_t, int>, bool> caps_;
};

void KsOutput::threadMain(std::promise<void>& opened) {
    {
        std::lock_guard lock(cmdMutex_);
        applied_ = false;
        threadDone_ = false;
    }
    HANDLE filter = path_.empty() ? INVALID_HANDLE_VALUE : openFilter(path_);
    if (filter == INVALID_HANDLE_VALUE) {
        opened.set_exception(std::make_exception_ptr(Error(ErrorCode::DeviceNotFound, "KS filter not found")));
        return;
    }
    PinInfo pi = pinInfo(filter, pinId_);
    const bool looped = useLooped(pi);
    HANDLE pin = nullptr;
    DWORD e = createPin(filter, pinId_, looped, fmt_, &pin);
    CloseHandle(filter);
    if (e != ERROR_SUCCESS || !pin) {
        char msg[96];
        const int length = snprintf(msg, sizeof msg, "KsCreatePin failed: %lu", e);
        if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "KsCreatePin failed");
        // ERROR_NO_MATCH means the format or interface is wrong; anything else is almost always another client holding the pin.
        opened.set_exception(
            std::make_exception_ptr(Error(e == ERROR_NO_MATCH ? ErrorCode::FormatNotSupported : ErrorCode::DeviceInUse, msg)));
        return;
    }
    timeBeginPeriod(1);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(kMmcssTask, &task);
    if (looped) {
        bool notify = cfg_.mode != KsMode::WaveRtPoll;
        runWaveRt(pin, notify, opened);
    } else {
        runStandard(pin, opened);
    }
    setState(pin, KSSTATE_PAUSE);
    setState(pin, KSSTATE_ACQUIRE);
    setState(pin, KSSTATE_STOP);
    CloseHandle(pin);
    if (av) AvRevertMmThreadCharacteristics(av);
    timeEndPeriod(1);
    std::lock_guard lock(cmdMutex_);
    threadDone_ = true;
    cmdCv_.notify_all();
}

bool KsOutput::waitRunning() {
    while (!quit_ && !running_) WaitForSingleObject(wake_, INFINITE);
    return !quit_;
}

void KsOutput::finish(DWORD err, const char* what) {
    if (!err || quit_) return;
    char msg[128];
    const int length = snprintf(msg, sizeof msg, "%s: %lu", what, err);
    if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "KS output failed");
    log::error("ks: %s", msg);
    raiseError(Error(ErrorCode::DeviceLost, msg));
}

void KsOutput::runWaveRt(HANDLE pin, bool notify, std::promise<void>& opened) {
    const UINT blk = fmt_.bytesPerFrame();
    const DWORD want = DWORD(fmt_.sampleRate * cfg_.bufferMs / 1000.0) * blk;
    KSRTAUDIO_BUFFER rb{};
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;
    if (notify) {
        KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION bp{};
        bp.Property = ksProp(kRtAudioSet, KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION, KSPROPERTY_TYPE_GET);
        bp.RequestedBufferSize = want;
        bp.NotificationCount = 2;
        ok = ksIoctl(pin, IOCTL_KS_PROPERTY, &bp, sizeof bp, &rb, sizeof rb);
        if (ok) {
            KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY np{};
            np.Property = ksProp(kRtAudioSet, KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET);
            np.NotificationEvent = ev;
            ok = ksIoctl(pin, IOCTL_KS_PROPERTY, &np, sizeof np, nullptr, 0);
        }
        if (!ok && cfg_.mode == KsMode::Auto) { // driver without notification support: fall back to polling
            log::info("ks: WaveRT notification unavailable (%lu), polling instead", GetLastError());
            notify = false;
        }
    }
    if (!notify) {
        KSRTAUDIO_BUFFER_PROPERTY bp{};
        bp.Property = ksProp(kRtAudioSet, KSPROPERTY_RTAUDIO_BUFFER, KSPROPERTY_TYPE_GET);
        bp.RequestedBufferSize = want;
        ok = ksIoctl(pin, IOCTL_KS_PROPERTY, &bp, sizeof bp, &rb, sizeof rb);
    }
    if (!ok || !rb.BufferAddress) {
        char msg[96];
        const int length = snprintf(msg, sizeof msg, "KSPROPERTY_RTAUDIO_BUFFER failed: %lu", GetLastError());
        if (length < 0 || size_t(length) >= sizeof msg) strcpy_s(msg, "KSPROPERTY_RTAUDIO_BUFFER failed");
        CloseHandle(ev);
        opened.set_exception(std::make_exception_ptr(Error(ErrorCode::DriverError, msg)));
        return;
    }
    auto base = static_cast<BYTE*>(rb.BufferAddress);
    const ULONG size = rb.ActualBufferSize / blk * blk;
    volatile ULONG* posReg = nullptr;
    KSRTAUDIO_HWREGISTER_PROPERTY hp{};
    hp.Property = ksProp(kRtAudioSet, KSPROPERTY_RTAUDIO_POSITIONREGISTER, KSPROPERTY_TYPE_GET);
    KSRTAUDIO_HWREGISTER hr{};
    if (ksIoctl(pin, IOCTL_KS_PROPERTY, &hp, sizeof hp, &hr, sizeof hr) && hr.Register)
        posReg = static_cast<volatile ULONG*>(hr.Register);
    char detail[128];
    const int detailLength = snprintf(detail, sizeof detail, "buffer %lu bytes (%.2f ms), position %s", rb.ActualBufferSize,
                                      rb.ActualBufferSize * 1000.0 / (fmt_.sampleRate * blk), posReg ? "register" : "property");
    setStatus(notify ? "KS · WaveRT event" : "KS · WaveRT polling", size / blk,
              detailLength < 0 || size_t(detailLength) >= sizeof detail ? "buffer status unavailable" : detail);
    opened.set_value();

    auto readPos = [&](ULONG& out) -> bool {
        if (posReg) {
            out = *posReg % size;
            return true;
        }
        KSPROPERTY ap = ksProp(KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, KSPROPERTY_TYPE_GET);
        KSAUDIO_POSITION apos{};
        if (!ksIoctl(pin, IOCTL_KS_PROPERTY, &ap, sizeof ap, &apos, sizeof apos)) return false;
        out = ULONG(apos.PlayOffset % size);
        return true;
    };
    auto fill = [&](ULONG from, ULONG to) { // circular [from, to)
        ULONG n = (to + size - from) % size;
        for (ULONG done = 0; done < n;) {
            ULONG at = (from + done) % size, chunk = std::min(n - done, size - at);
            size_t got = source_->render(base + at, chunk / blk);
            if (got * blk < chunk) memset(base + at + got * blk, 0, chunk - got * blk);
            done += chunk;
        }
        if (rb.CallMemoryBarrier) MemoryBarrier();
    };

    DWORD err = 0;
    const DWORD sleepMs = std::max<DWORD>(1, DWORD(size * 1000.0 / (fmt_.sampleRate * blk) / 4));
    bool runningState = false;
    ULONG wr = 0;
    while (!quit_) {
        if (!running_) {
            if (runningState) {
                setState(pin, KSSTATE_PAUSE);
                runningState = false;
            }
            notifyApplied(false);
            if (!waitRunning()) break;
            continue;
        }
        if (!runningState) {
            // (Re)start: fill everything except one frame, then run from the start of the buffer.
            setState(pin, KSSTATE_STOP);
            fill(0, size - blk);
            wr = size - blk;
            setState(pin, KSSTATE_ACQUIRE);
            setState(pin, KSSTATE_PAUSE);
            if (!setState(pin, KSSTATE_RUN)) {
                err = GetLastError();
                break;
            }
            runningState = true;
            notifyApplied(true);
        }
        if (notify) {
            HANDLE hs[2] = {ev, wake_};
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
            if (w == WAIT_TIMEOUT) {
                err = ERROR_TIMEOUT;
                break;
            }
            if (w == WAIT_OBJECT_0 + 1) continue;
        } else {
            WaitForSingleObject(wake_, sleepMs);
        }
        ULONG pos = 0;
        if (!readPos(pos)) {
            err = GetLastError();
            break;
        }
        pos = pos / blk * blk;
        ULONG target = (pos + size - blk) % size; // keep the writer one frame behind the play cursor
        fill(wr, target);
        wr = target;
    }
    if (notify) {
        KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY np{};
        np.Property = ksProp(kRtAudioSet, KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET);
        np.NotificationEvent = ev;
        ksIoctl(pin, IOCTL_KS_PROPERTY, &np, sizeof np, nullptr, 0);
    }
    CloseHandle(ev);
    finish(err, "WaveRT stream");
}

void KsOutput::runStandard(HANDLE pin, std::promise<void>& opened) {
    const int n = std::clamp(cfg_.packetCount, 2, 16);
    const UINT blk = fmt_.bytesPerFrame();
    const DWORD pktFrames = std::max<DWORD>(64, DWORD(fmt_.sampleRate * cfg_.packetMs / 1000.0));
    const DWORD pktBytes = pktFrames * blk;
    std::vector<std::vector<BYTE>> bufs(n, std::vector<BYTE>(pktBytes));
    std::vector<KSSTREAM_HEADER> hdr(n);
    std::vector<OVERLAPPED> ov(n);
    for (auto& o : ov) {
        o = {};
        o.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    char detail[96];
    const int detailLength =
        snprintf(detail, sizeof detail, "%d packets × %lu frames (%.1f ms)", n, pktFrames, pktFrames * 1000.0 / fmt_.sampleRate);
    setStatus("KS · standard streaming", pktFrames * n,
              detailLength < 0 || size_t(detailLength) >= sizeof detail ? "streaming buffer status unavailable" : detail);
    opened.set_value();

    auto submit = [&](int i) -> bool {
        size_t got = source_->render(bufs[i].data(), pktFrames);
        if (got < pktFrames) memset(bufs[i].data() + got * blk, 0, (pktFrames - got) * blk);
        hdr[i] = {};
        hdr[i].Size = sizeof(KSSTREAM_HEADER);
        hdr[i].PresentationTime.Numerator = 1;
        hdr[i].PresentationTime.Denominator = 1;
        hdr[i].FrameExtent = pktBytes;
        hdr[i].DataUsed = pktBytes;
        hdr[i].Data = bufs[i].data();
        ResetEvent(ov[i].hEvent);
        BOOL ok = DeviceIoControl(pin, IOCTL_KS_WRITE_STREAM, nullptr, 0, &hdr[i], sizeof hdr[i], nullptr, &ov[i]);
        return ok || GetLastError() == ERROR_IO_PENDING;
    };
    auto drain = [&] { // STOP cancels queued packets; wait for their completion
        setState(pin, KSSTATE_PAUSE);
        setState(pin, KSSTATE_ACQUIRE);
        setState(pin, KSSTATE_STOP);
        for (auto& o : ov) WaitForSingleObject(o.hEvent, 1000);
    };

    DWORD err = 0;
    bool runningState = false;
    int cur = 0;
    while (!quit_) {
        if (!running_) {
            if (runningState) {
                drain();
                runningState = false;
            }
            notifyApplied(false);
            if (!waitRunning()) break;
            continue;
        }
        if (!runningState) {
            setState(pin, KSSTATE_ACQUIRE);
            setState(pin, KSSTATE_PAUSE);
            for (int i = 0; i < n && !err; i++)
                if (!submit(i)) err = GetLastError();
            if (err || !setState(pin, KSSTATE_RUN)) {
                if (!err) err = GetLastError();
                break;
            }
            cur = 0;
            runningState = true;
            notifyApplied(true);
        }
        HANDLE hs[2] = {ov[cur].hEvent, wake_};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
        if (w == WAIT_OBJECT_0 + 1) continue;
        if (w == WAIT_TIMEOUT) {
            err = ERROR_TIMEOUT;
            break;
        }
        DWORD got = 0;
        if (!GetOverlappedResult(pin, &ov[cur], &got, FALSE)) {
            err = GetLastError();
            break;
        }
        if (!submit(cur)) {
            err = GetLastError();
            break;
        }
        cur = (cur + 1) % n;
    }
    if (runningState) drain();
    for (auto& o : ov) CloseHandle(o.hEvent);
    finish(err, "standard stream");
}

} // namespace

std::unique_ptr<IOutput> createKsOutput(const KsSettings& s) {
    return std::make_unique<KsOutput>(s);
}

std::vector<DeviceEntry> listKsPins() {
    std::vector<DeviceEntry> out;
    for (auto& f : renderFilters()) {
        HANDLE h = openFilter(f.path);
        if (h == INVALID_HANDLE_VALUE) continue;
        KSPROPERTY p = ksProp(KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET);
        ULONG n = 0;
        ksIoctl(h, IOCTL_KS_PROPERTY, &p, sizeof p, &n, sizeof n);
        for (ULONG pin = 0; pin < n; pin++) {
            PinInfo pi = pinInfo(h, pin);
            if (!pi.pcmRender) continue;
            std::string name = f.name;
            if (!pi.name.empty()) name += " · " + pi.name;
            name += f.realtime && pi.looped ? " [WaveRT]" : " [Standard]";
            out.push_back({toUtf8(f.path) + "|" + std::to_string(pin), name});
        }
        CloseHandle(h);
    }
    return out;
}

} // namespace camel::audio
