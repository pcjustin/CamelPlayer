// Persisted settings (docs/Plan.md §6, §16.4). One set per backend; stored as JSON in %LOCALAPPDATA%\CamelPlayer.
#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "Common/AudioFormat.h"
#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"

namespace camel {

enum class Backend : uint8_t { Wasapi, Asio, Ks };
const char* backendId(Backend b); // "wasapi" / "asio" / "ks"
const char* backendName(Backend b);
bool backendFromId(std::string_view id, Backend& out);

enum class WasapiPeriod : uint8_t { Default, Minimum, Custom };

// Raw DSD output (docs/Plan.md §5.8, §6). Native falls back to DoP, never to PCM. WASAPI plays DoP only; KS stores
// the choice but plays DoP until native DSD on KS pins is verified.
enum class DsdOutput : uint8_t { Native, Dop };
const char* dsdOutputId(DsdOutput d);

struct WasapiSettings {
    std::string deviceId; // endpoint id (UTF-8); empty = Windows default device
    bool eventDriven = true;
    WasapiPeriod period = WasapiPeriod::Default;
    double customPeriodMs = 10.0;
    std::optional<audio::SampleType> sampleType; // nullopt = automatic
    bool operator==(const WasapiSettings&) const = default;
};

struct AsioSettings {
    std::string driverClsid; // empty = first installed driver
    long bufferSize = 0;     // 0 = driver's preferred size
    int outLeft = 0, outRight = 1;
    DsdOutput dsdOutput = DsdOutput::Native;
    bool operator==(const AsioSettings&) const = default;
};

enum class KsMode : uint8_t { Auto, WaveRtEvent, WaveRtPoll, Standard };
const char* ksModeId(KsMode m);

// soxr quick / high / very high (docs/Plan.md §6).
enum class ResampleQuality : uint8_t { Quick, High, VeryHigh };
const char* resampleQualityId(ResampleQuality q);

struct KsSettings {
    std::string deviceId; // "<filter path>|<pin id>"; empty = first PCM render pin
    KsMode mode = KsMode::Auto;
    double bufferMs = 20.0; // WaveRT ring buffer
    int packetCount = 4;    // standard streaming
    double packetMs = 10.0;
    std::optional<audio::SampleType> sampleType;
    DsdOutput dsdOutput = DsdOutput::Dop;
    bool operator==(const KsSettings&) const = default;
};

// Explicit per-format choices; a format without one uses its default (docs/Plan.md §3.1).
template <class T> struct PerFormat {
    std::array<std::optional<T>, std::size(audio::kAllFormats)> choices{};
    std::optional<T>& operator[](audio::AudioFormat f) { return choices[size_t(f)]; }
    const std::optional<T>& operator[](audio::AudioFormat f) const { return choices[size_t(f)]; }
    bool operator==(const PerFormat&) const = default;
};
using DecoderByFormat = PerFormat<audio::DecoderKind>;
using PcmBitsByFormat = PerFormat<uint32_t>; // lossy formats only: 16 or 24

// Precondition: audio::decodersFor(format) is not empty.
audio::DecoderKind decoderFor(const DecoderByFormat& choices, audio::AudioFormat format);
// The PCM depth a lossy format is decoded to; 24 unless set (docs/Plan.md §2).
uint32_t pcmBitsFor(const PcmBitsByFormat& choices, audio::AudioFormat format);

struct Settings {
    static constexpr int kSchemaVersion = 3;

    Backend backend = Backend::Wasapi;
    DecoderByFormat decoderByFormat;
    PcmBitsByFormat pcmBitsByFormat;
    // "Convert when unsupported" (docs/Plan.md §2, §5.3): resample, downmix or reduce depth instead of skipping.
    bool convertWhenUnsupported = false;
    ResampleQuality resampleQuality = ResampleQuality::High;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    std::string language; // "" = follow Windows, else "zh-TW" / "en-US"
    std::wstring lastFile;
};

// Fixed in the MVP (docs/Plan.md §16.4).
constexpr const wchar_t* kMmcssTask = L"Pro Audio";

std::filesystem::path dataDirectory(); // %LOCALAPPDATA%\CamelPlayer (created on demand)
std::filesystem::path settingsPath();

// Missing or corrupt file -> defaults (the corrupt file is kept as settings.json.bad).
Settings loadSettings(const std::filesystem::path& path = settingsPath());
// Atomic replace (write temp file, then MoveFileEx).
void saveSettings(const Settings& s, const std::filesystem::path& path = settingsPath());

std::string toJson(const Settings& s);
Settings fromJson(const std::string& json); // throws on malformed JSON

std::optional<audio::SampleType> sampleTypeFromId(std::string_view id); // "16", "24", "24in32", "32", "f32"; "" = auto
const char* sampleTypeId(audio::SampleType t);

} // namespace camel
