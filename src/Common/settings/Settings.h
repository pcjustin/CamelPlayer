// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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

// DSD output (docs/Plan.md §5.8, §6). Native falls back to DoP; PCM is used only when chosen here or when
// Settings::convertDsdWhenUnsupported allows it after native/DoP failed. Only ASIO has native DSD; WASAPI and KS
// (Windows defines no DSD format for either) read a stored Native as DoP.
enum class DsdOutput : uint8_t { Native, Dop, Pcm };
// DSD to PCM output rates (docs/Plan.md §6): 88.2, 176.4 (default) or 352.8 kHz.
constexpr uint32_t kDsdPcmRates[] = {88200, 176400, 352800};
const char* dsdOutputId(DsdOutput d);

// soxr quick / high / very high (docs/Plan.md §6).
enum class ResampleQuality : uint8_t { Quick, High, VeryHigh };
const char* resampleQualityId(ResampleQuality q);

// Fixed output rates (docs/Plan.md §6): 0 follows the file.
constexpr uint32_t kFixedRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};

enum class MmcssTask : uint8_t { ProAudio, Playback, Disabled };
enum class MmcssPriority : uint8_t { Normal, High, Critical };
const wchar_t* mmcssTaskName(MmcssTask t); // nullptr for Disabled

// Engine behavior each backend keeps its own copy of (docs/Plan.md §6). Changing it never reopens a playing stream;
// it applies from the next stream (predecode) or the next pause / format change.
struct PlaybackTiming {
    uint32_t predecodeSeconds = 10;
    uint32_t formatChangeSilenceMs = 0;
    uint32_t releaseAfterPauseSeconds = 60; // 0 = never
    bool operator==(const PlaybackTiming&) const = default;
};

struct WasapiSettings {
    std::string deviceId; // endpoint id (UTF-8); empty = follow the Windows default device
    // Shared mode plays through the Windows mixer: always converted to its mix format and shown as processed. Choosing
    // it is the permission to convert (docs/Plan.md §5.4).
    bool shared = false;
    ResampleQuality sharedResampleQuality = ResampleQuality::High;
    bool eventDriven = true;
    WasapiPeriod period = WasapiPeriod::Default;
    double customPeriodMs = 10.0;
    std::optional<audio::SampleType> sampleType; // nullopt = automatic
    DsdOutput dsdOutput = DsdOutput::Dop;
    uint32_t dsdPcmRate = 176400;
    // Exclusive mode only; choosing a rate is the permission to resample PCM to it (raw DSD is never resampled).
    uint32_t fixedRate = 0;
    double pollBufferMs = 200.0;
    MmcssTask mmcssTask = MmcssTask::ProAudio;
    MmcssPriority mmcssPriority = MmcssPriority::High;
    PlaybackTiming timing;
    bool operator==(const WasapiSettings&) const = default;
};

struct AsioSettings {
    std::string driverClsid; // empty = first installed driver
    long bufferSize = 0;     // 0 = driver's preferred size
    int outLeft = 0, outRight = 1;
    DsdOutput dsdOutput = DsdOutput::Native;
    uint32_t dsdPcmRate = 176400;
    uint32_t fixedRate = 0;
    PlaybackTiming timing;
    bool operator==(const AsioSettings&) const = default;
};

enum class KsMode : uint8_t { Auto, WaveRtEvent, WaveRtPoll, Standard };
const char* ksModeId(KsMode m);

struct KsSettings {
    std::string deviceId; // "<filter path>|<pin id>"; empty = first PCM render pin
    KsMode mode = KsMode::Auto;
    double bufferMs = 20.0; // WaveRT ring buffer
    int packetCount = 4;    // standard streaming
    double packetMs = 10.0;
    std::optional<audio::SampleType> sampleType;
    DsdOutput dsdOutput = DsdOutput::Dop;
    uint32_t dsdPcmRate = 176400;
    uint32_t fixedRate = 0;
    MmcssTask mmcssTask = MmcssTask::ProAudio;
    MmcssPriority mmcssPriority = MmcssPriority::High;
    PlaybackTiming timing;
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

// What closing the main window does (docs/Plan.md §8.4).
enum class CloseAction : uint8_t { Tray, Exit };

struct Settings {
    static constexpr int kSchemaVersion = 3;

    Backend backend = Backend::Wasapi;
    DecoderByFormat decoderByFormat;
    PcmBitsByFormat pcmBitsByFormat;
    // "Convert when unsupported" (docs/Plan.md §2, §5.3): resample, downmix or reduce depth instead of skipping.
    bool convertWhenUnsupported = false;
    ResampleQuality resampleQuality = ResampleQuality::High;
    // "Convert DSD when unsupported" (docs/Plan.md §5.3 item 4): DSD to PCM after native/DoP failed. Independent of
    // convertWhenUnsupported, which still governs rates, downmix and depth of the PCM it produces.
    bool convertDsdWhenUnsupported = false;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    std::string language; // "" = follow Windows, else "zh-TW" / "en-US"
    CloseAction closeAction = CloseAction::Tray;
    bool trayNoticeShown = false; // the first close to the tray has explained where CamelPlayer went
    bool firstRunDone = false;    // the first-run guide was finished or skipped
    bool autoplay = true;         // when the queue ends, random library tracks keep playing (docs/Plan.md §8.1)
    std::wstring lastFile;
};

// Settings › Reset (docs/Plan.md §8.2). Playback and output: backend, devices, every backend parameter, conversion
// and decoders. Interface: language, close action, the tray notice and the first-run guide. Each keeps the other group.
Settings resetPlayback(const Settings& s);
Settings resetInterface(const Settings& s);

// The selected backend's fixed rate (0: follow the file) and timing.
uint32_t fixedRate(const Settings& s);
const PlaybackTiming& timing(const Settings& s);

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
