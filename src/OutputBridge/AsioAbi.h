// The binary interface of ASIO drivers as a host sees it (docs/Plan.md §5.6), written for interoperability from the
// interface's layout and values; it contains no Steinberg SDK code. x64 only: COM methods and driver callbacks use the
// single x64 calling convention. ASIO is a trademark of Steinberg Media Technologies GmbH.
#pragma once
#include <unknwn.h>

#include <cstdint>

namespace camel::audio::asio {

using Bool = long;
using Status = long;
using SampleRate = double;
using SampleType = long;

constexpr Bool kFalse = 0;
constexpr Bool kTrue = 1;

constexpr Status kOk = 0;
constexpr Status kSuccess = 0x3f4847a0; // returned by `future` for a supported selector

namespace sample {
constexpr SampleType kInt16Lsb = 16;
constexpr SampleType kInt24Lsb = 17;
constexpr SampleType kInt32Lsb = 18;
constexpr SampleType kFloat32Lsb = 19;
constexpr SampleType kFloat64Lsb = 20;
constexpr SampleType kInt32Lsb16 = 24;
constexpr SampleType kInt32Lsb18 = 25;
constexpr SampleType kInt32Lsb20 = 26;
constexpr SampleType kInt32Lsb24 = 27;
constexpr SampleType kDsdInt8Lsb1 = 32;
constexpr SampleType kDsdInt8Msb1 = 33;
constexpr SampleType kDsdInt8Ner8 = 40;
} // namespace sample

// Selectors the driver passes to the host's message callback.
namespace message {
constexpr long kSelectorSupported = 1;
constexpr long kEngineVersion = 2;
constexpr long kResetRequest = 3;
constexpr long kBufferSizeChange = 4;
constexpr long kResyncRequest = 5;
constexpr long kLatenciesChanged = 6;
constexpr long kSupportsTimeInfo = 7;
} // namespace message

// Selectors for Driver::future.
namespace future {
constexpr long kSetIoFormat = 0x23111961;
constexpr long kGetIoFormat = 0x23111983;
constexpr long kCanDoIoFormat = 0x23112004;
} // namespace future

enum class IoFormatType : long { Pcm = 0, Dsd = 1 };

struct IoFormat {
    IoFormatType type;
    char reserved[512 - sizeof(long)];
};

struct Samples {
    unsigned long hi, lo;
};

struct ChannelInfo {
    long channel;
    Bool isInput;
    Bool isActive;
    long channelGroup;
    SampleType type;
    char name[32];
};

struct BufferInfo {
    Bool isInput;
    long channel;
    void* buffers[2];
};

struct Time; // only passed through by this host

struct Callbacks {
    void (*bufferSwitch)(long index, Bool processNow);
    void (*sampleRateChanged)(SampleRate rate);
    long (*message)(long selector, long value, void* data, double* extra);
    Time* (*bufferSwitchTimeInfo)(Time* time, long index, Bool processNow);
};

// The driver object, created with CoCreateInstance(clsid, ..., clsid). Method order is the vtable order.
struct Driver : IUnknown {
    virtual Bool init(void* window) = 0;
    virtual void getDriverName(char* name) = 0; // at least 32 bytes
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* text) = 0; // at least 124 bytes
    virtual Status start() = 0;
    virtual Status stop() = 0;
    virtual Status getChannels(long* inputs, long* outputs) = 0;
    virtual Status getLatencies(long* input, long* output) = 0;
    virtual Status getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) = 0;
    virtual Status canSampleRate(SampleRate rate) = 0;
    virtual Status getSampleRate(SampleRate* rate) = 0;
    virtual Status setSampleRate(SampleRate rate) = 0;
    virtual Status getClockSources(void* sources, long* count) = 0;
    virtual Status setClockSource(long reference) = 0;
    virtual Status getSamplePosition(Samples* position, Samples* timeStamp) = 0;
    virtual Status getChannelInfo(ChannelInfo* info) = 0;
    virtual Status createBuffers(BufferInfo* infos, long count, long bufferSize, Callbacks* callbacks) = 0;
    virtual Status disposeBuffers() = 0;
    virtual Status controlPanel() = 0;
    virtual Status future(long selector, void* options) = 0;
    virtual Status outputReady() = 0;
};

static_assert(sizeof(void*) == 8, "ASIO host ABI is written for x64");
static_assert(sizeof(long) == 4 && sizeof(IoFormat) == 512);
static_assert(sizeof(ChannelInfo) == 52 && sizeof(BufferInfo) == 24 && sizeof(Callbacks) == 32);

} // namespace camel::audio::asio
