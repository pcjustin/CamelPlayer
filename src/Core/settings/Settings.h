// Persisted settings (docs/計劃書.md §6, §16.4). One set per backend; stored as JSON in %LOCALAPPDATA%\CamelPlayer.
#pragma once
#include <filesystem>
#include <optional>
#include <string>

#include "audio/AudioTypes.h"
#include "audio/decode/IDecoder.h"

namespace camel {

enum class Backend : uint8_t { Wasapi, Asio, Ks };
const char* backendId(Backend b);  // "wasapi" / "asio" / "ks"
const char* backendName(Backend b);
bool backendFromId(std::string_view id, Backend& out);

enum class WasapiPeriod : uint8_t { Default, Minimum, Custom };

struct WasapiSettings {
    std::string deviceId;  // endpoint id (UTF-8); empty = Windows default device
    bool eventDriven = true;
    WasapiPeriod period = WasapiPeriod::Default;
    double customPeriodMs = 10.0;
    std::optional<audio::SampleType> sampleType;  // nullopt = automatic
    bool operator==(const WasapiSettings&) const = default;
};

struct AsioSettings {
    std::string driverClsid;  // empty = first installed driver
    long bufferSize = 0;      // 0 = driver's preferred size
    int outLeft = 0, outRight = 1;
    bool operator==(const AsioSettings&) const = default;
};

enum class KsMode : uint8_t { Auto, WaveRtEvent, WaveRtPoll, Standard };
const char* ksModeId(KsMode m);

struct KsSettings {
    std::string deviceId;  // "<filter path>|<pin id>"; empty = first PCM render pin
    KsMode mode = KsMode::Auto;
    double bufferMs = 20.0;        // WaveRT ring buffer
    int packetCount = 4;           // standard streaming
    double packetMs = 10.0;
    std::optional<audio::SampleType> sampleType;
    bool operator==(const KsSettings&) const = default;
};

struct Settings {
    static constexpr int kSchemaVersion = 1;

    Backend backend = Backend::Wasapi;
    audio::DecoderKind decoder = audio::DecoderKind::FFmpeg;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    std::string language;  // "" = follow Windows, else "zh-TW" / "en-US"
    std::wstring lastFile;
};

// Fixed in the MVP (docs/計劃書.md §16.4).
constexpr double kPrebufferSeconds = 10.0;
constexpr const wchar_t* kMmcssTask = L"Pro Audio";

std::filesystem::path dataDirectory();  // %LOCALAPPDATA%\CamelPlayer (created on demand)
std::filesystem::path settingsPath();

// Missing or corrupt file -> defaults (the corrupt file is kept as settings.json.bad).
Settings loadSettings(const std::filesystem::path& path = settingsPath());
// Atomic replace (write temp file, then MoveFileEx).
void saveSettings(const Settings& s, const std::filesystem::path& path = settingsPath());

std::string toJson(const Settings& s);
Settings fromJson(const std::string& json);  // throws on malformed JSON

std::optional<audio::SampleType> sampleTypeFromId(std::string_view id);  // "16", "24", "24in32", "32", "f32"; "" = auto
const char* sampleTypeId(audio::SampleType t);

}  // namespace camel
