// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Persisted settings (docs/Plan.md §6, §16.4). One parameter set per output device; stored as JSON in
// %LOCALAPPDATA%\CamelPlayer.
#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Common/AudioFormat.h"
#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"

namespace camel {

enum class Backend : uint8_t { Wasapi, Asio, Ks, Network, AirPlay };
const char* backendId(Backend b); // "wasapi" / "asio" / "ks" / "network" / "airplay"
const char* backendName(Backend b);
inline bool isNetworkPlayer(Backend b) {
    return b == Backend::Network || b == Backend::AirPlay;
}
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

// Fixed output rates (docs/Plan.md §6): 0 follows the file. Also the choices of an output's highest PCM rate.
constexpr uint32_t kFixedRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
// The choices of an output's highest DSD rate, as multiples of 44.1 kHz: DSD64, DSD128 and DSD256.
constexpr uint32_t kMaxDsdRates[] = {64, 128, 256};
// The DSD byte frame rate of DSD`multiple`: 352800 for DSD64.
constexpr uint32_t dsdByteRate(uint32_t multiple) {
    return multiple / 64 * 352800;
}

enum class MmcssTask : uint8_t { ProAudio, Playback, Disabled };
enum class MmcssPriority : uint8_t { Normal, High, Critical };
const wchar_t* mmcssTaskName(MmcssTask t); // nullptr for Disabled

// Engine behavior each backend keeps its own copy of (docs/Plan.md §6). Changing it never reopens a playing stream;
// it applies from the next stream (predecode) or the next pause / format change.
struct PlaybackTiming {
    uint32_t predecodeSeconds = 10;
    uint32_t formatChangeSilenceMs = 0;
    uint32_t releaseAfterPauseSeconds = 1; // 0 = never
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
    // The highest PCM rate (from kFixedRates) and DSD rate (from kMaxDsdRates) to send, 0 following the device;
    // exclusive mode only. Choosing one is the permission to convert above it (docs/Plan.md §6).
    uint32_t maxSampleRate = 0;
    uint32_t maxDsdRate = 0;
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
    uint32_t maxSampleRate = 0; // as for WASAPI
    uint32_t maxDsdRate = 0;
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
    uint32_t maxSampleRate = 0; // as for WASAPI
    uint32_t maxDsdRate = 0;
    MmcssTask mmcssTask = MmcssTask::ProAudio;
    MmcssPriority mmcssPriority = MmcssPriority::High;
    PlaybackTiming timing;
    bool operator==(const KsSettings&) const = default;
};

// A UPnP AV or OpenHome player (docs/Plan.md §5.10), kept by its UDN and never by its address.
struct NetworkSettings {
    std::string udn;
    uint32_t maxSampleRate = 0;              // as for WASAPI
    uint32_t maxDsdRate = 0;                 // as for WASAPI
    uint32_t maxBits = 0;                    // 0 follows the player, else 16
    DsdOutput dsdOutput = DsdOutput::Native; // Native is DSF; Pcm is the permission to convert
    uint32_t dsdPcmRate = 176400;
    bool gapless = true;                                   // off for a player that declares a next track and handles it badly
    PlaybackTiming timing{.releaseAfterPauseSeconds = 60}; // a player resumes slower than a local device
    bool operator==(const NetworkSettings&) const = default;
};

// What an AirPlay receiver plays: both channels, or one of them in both of its own, so that a single speaker plays
// the whole of it or two make a stereo pair (docs/Plan.md §5.10). A stereo pair always plays both.
enum class AirPlayChannels : uint8_t { Stereo, Left, Right };

// The format is always 44.1 kHz/16-bit, so a receiver has no limit, DSD or silence settings (docs/Plan.md §6).
struct AirPlaySettings {
    std::string deviceId;
    AirPlayChannels channels = AirPlayChannels::Stereo;
    PlaybackTiming timing{.releaseAfterPauseSeconds = 60};
    bool operator==(const AirPlaySettings&) const = default;
};

// An AirPlay 1 receiver cannot be asked for its level, so the last one is kept for each (docs/Plan.md §6).
struct AirPlayLevel {
    uint32_t level = 33; // about -20 dB
    bool muted = false;
    bool operator==(const AirPlayLevel&) const = default;
};

// Explicit per-format choices; a format without one uses its default (docs/Plan.md §3.1).
template <class T> struct PerFormat {
    std::array<std::optional<T>, std::size(audio::kAllFormats)> choices{};
    std::optional<T>& operator[](audio::AudioFormat f) { return choices[size_t(f)]; }
    const std::optional<T>& operator[](audio::AudioFormat f) const { return choices[size_t(f)]; }
    bool operator==(const PerFormat&) const = default;
};
using DecoderByFormat = PerFormat<audio::DecoderKind>;

// Precondition: audio::decodersFor(format) is not empty.
audio::DecoderKind decoderFor(const DecoderByFormat& choices, audio::AudioFormat format);

// What closing the main window does (docs/Plan.md §8.4).
enum class CloseAction : uint8_t { Tray, Exit };
// The window's colours: the Windows app mode, or always light or dark.
enum class Theme : uint8_t { System, Light, Dark };
// A lookup on the internet the user is asked about once, where it is first needed.
enum class OnlineLookup : uint8_t { Ask, Allowed, Refused };

// A global hotkey as RegisterHotKey takes it: MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8 and a virtual-key code;
// key 0 leaves the action without a hotkey.
struct Hotkey {
    uint32_t modifiers = 0;
    uint32_t key = 0;
    bool operator==(const Hotkey&) const = default;
};

// Global hotkeys (docs/Plan.md §8.4): Alt+Home, Alt+Page Up and Alt+Page Down by default; Alt+Space and Alt+arrows
// stay with every program (system menu, back and forward).
struct HotkeySettings {
    Hotkey playPause{1, 0x24};
    Hotkey previous{1, 0x21};
    Hotkey next{1, 0x22};
    bool operator==(const HotkeySettings&) const = default;
};

// A parametric EQ band (docs/Plan.md §5.9).
enum class EqKind : uint8_t { Peaking = 1, LowShelf, HighShelf, LowPass, HighPass, BandPass, Notch, AllPass };
enum class EqChannels : uint8_t { All, Left, Right };

struct EqBand {
    EqKind kind = EqKind::Peaking;
    EqChannels channels = EqChannels::All;
    uint32_t slope = 12; // low- and high-pass dB per octave, 6 to 48 in steps of 6; 12 uses q
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.71;
    bool enabled = true;
    bool operator==(const EqBand&) const = default;
};

struct EqPreset {
    std::string name;
    std::vector<EqBand> bands;
    bool operator==(const EqPreset&) const = default;
};

// The 5 flat bands a new EQ starts with: low shelf 100 Hz; peaking 400 Hz, 1 kHz and 4 kHz; high shelf 10 kHz.
std::vector<EqBand> defaultEqBands();

// One global EQ (docs/Plan.md §5.9), off by default. The Engine takes any number of bands; the UI adds up to
// kMaxUiBands for now.
struct EqSettings {
    static constexpr size_t kMaxUiBands = 10;
    bool enabled = false;
    std::vector<EqBand> bands = defaultEqBands();
    std::string preset; // the saved preset last chosen, "" for none
    std::vector<EqPreset> presets;
    bool operator==(const EqSettings&) const = default;
};

// Headphone crossfeed (docs/Plan.md §5.9): bs2b, off by default. A preset sets the cut-off and feed; Custom uses the
// values here.
enum class CrossfeedPreset : uint8_t { Default, ChuMoy, JanMeier, Custom };

struct CrossfeedSettings {
    static constexpr uint32_t kMinCutoffHz = 300, kMaxCutoffHz = 2000;
    static constexpr double kMinFeedDb = 1.0, kMaxFeedDb = 15.0;
    bool enabled = false;
    CrossfeedPreset preset = CrossfeedPreset::Default;
    uint32_t cutoffHz = 700; // Custom
    double feedDb = 4.5;     // Custom
    bool operator==(const CrossfeedSettings&) const = default;
};

// The cut-off in Hz and the feed in dB that `crossfeed` uses: its preset's, or its own for Custom.
std::pair<uint32_t, double> crossfeedValues(const CrossfeedSettings& crossfeed);

// Convolution with an impulse response (docs/Plan.md §5.9), off by default: a .wav, or a .zip of .wav files by rate.
struct ConvolutionSettings {
    bool enabled = false;
    std::string file; // UTF-8 path
    bool operator==(const ConvolutionSettings&) const = default;
};

// Software volume (docs/Plan.md §5.9): Fixed by default, leaving the level to the DAC or amplifier.
// Player is a network player's own volume, read from it and never stored (docs/Plan.md §6).
enum class VolumeMode : uint8_t { Fixed, Software, Player };

struct VolumeSettings {
    static constexpr double kMinDb = -80.0;
    VolumeMode mode = VolumeMode::Fixed;
    double db = 0.0; // -80 to 0
    bool muted = false;
    bool operator==(const VolumeSettings&) const = default;
};

// The volume the DSP chain applies in dB: 0 while Fixed, negative infinity while muted.
double volumeDb(const VolumeSettings& volume);

// ReplayGain (docs/Plan.md §5.9), off by default. Tracks without a value in their tags are analyzed in the background
// while it is on, at the speed set here.
enum class ReplayGainMode : uint8_t { Off, Track, Album, Auto };
enum class AnalysisSpeed : uint8_t { Throttled, Fast };

struct ReplayGainSettings {
    static constexpr double kMinPreampDb = -15.0, kMaxPreampDb = 15.0;
    ReplayGainMode mode = ReplayGainMode::Off;
    double preampDb = 0.0;        // tracks with a value
    double noValuePreampDb = 0.0; // tracks without one
    bool preventClipping = true;
    AnalysisSpeed speed = AnalysisSpeed::Throttled;
    bool operator==(const ReplayGainSettings&) const = default;
};

// Crossfade (docs/Plan.md §5.4), off by default; an album played in order stays gapless.
struct CrossfadeSettings {
    static constexpr double kMinSeconds = 1.0, kMaxSeconds = 10.0;
    bool enabled = false;
    double seconds = 3.0; // in 0.5 s steps
    bool operator==(const CrossfadeSettings&) const = default;
};

// Discord Rich Presence (docs/Plan.md §8.4): both off by default, since each sends something out.
struct DiscordSettings {
    bool enabled = false; // what is playing, to the local Discord app
    bool covers = false;  // the file's own cover, through a Cloudflare quick tunnel
    bool operator==(const DiscordSettings&) const = default;
};

// The title, artist and album columns of track lists as proportions of the room they share, which the user changes by
// dragging the dividers between their headers (docs/Plan.md §8.2).
struct TrackColumns {
    double title = 3, artist = 2, album = 2;
    bool operator==(const TrackColumns&) const = default;
};

// An output device the user enabled, named or set up, kept by stable identity (docs/Plan.md §6). Only `backend`'s
// parameters are used; those of the output in use live in Settings and are copied here as they change.
struct SavedOutput {
    Backend backend = Backend::Wasapi;
    std::string id;       // as in the backend's settings; empty on WASAPI is "Windows default device", without parameters
    std::string name;     // as the device last called itself, for when it is not connected
    std::string alias;    // the user's name for it; empty shows `name`
    bool enabled = false; // listed in the output menu
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    NetworkSettings network;
    AirPlaySettings airplay;
    VolumeMode volumeMode = VolumeMode::Fixed;
    // An AirPlay stereo pair's speakers by their own device IDs, which stay when pairing it again gives it a new ID.
    std::vector<std::string> speakers;
    bool operator==(const SavedOutput&) const = default;
};

struct Settings {
    static constexpr int kSchemaVersion = 4;

    Backend backend = Backend::Wasapi;
    DecoderByFormat decoderByFormat;
    // "Convert when unsupported" (docs/Plan.md §2, §5.3): resample, downmix, reduce depth or decode DTS instead of
    // skipping. On by default; the signal path then shows the conversion, and turning it off plays bit-perfect only.
    bool convertWhenUnsupported = true;
    ResampleQuality resampleQuality = ResampleQuality::High;
    // "Convert DSD when unsupported" (docs/Plan.md §5.3 item 4): DSD to PCM after native/DoP failed. Independent of
    // convertWhenUnsupported, which still governs rates, downmix and depth of the PCM it produces.
    bool convertDsdWhenUnsupported = false;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    NetworkSettings network;
    AirPlaySettings airplay;
    std::string language; // "" = follow Windows, else "zh-TW" / "en-US"
    CloseAction closeAction = CloseAction::Tray;
    Theme theme = Theme::System;
    bool trayNoticeShown = false; // the first close to the tray has explained where CamelPlayer went
    bool firstRunDone = false;    // the first-run guide was finished or skipped
    bool autoplay = true;         // when the queue ends, random library tracks keep playing (docs/Plan.md §8.1)
    bool folderPlaysOn = false;   // a folder, or a track in one, played goes on to the end of its library folder
    bool checkUpdates = true;     // automatic update checks at startup and daily (docs/Plan.md §8.6)
    OnlineLookup artistPictures = OnlineLookup::Ask; // from Wikimedia Commons (docs/Plan.md §7.4)
    bool folderTree = false; // the Folders page as one tree, as in foobar2000 (made for a friend of the user)
    TrackColumns trackColumns;
    DiscordSettings discord;
    std::string skippedVersion; // the release the automatic check no longer offers
    HotkeySettings hotkeys;
    EqSettings eq;
    CrossfeedSettings crossfeed;
    ConvolutionSettings convolution;
    audio::DspOrder dspOrder = audio::kDefaultDspOrder;
    VolumeSettings volume;
    ReplayGainSettings replayGain;
    CrossfadeSettings crossfade;
    std::vector<SavedOutput> outputs;
    // The AirPlay receivers playing together, the one in `airplay` among them; kept when another output is chosen, so
    // choosing any of them again brings the group back.
    std::vector<std::string> airplayGroup;
    std::map<std::string, AirPlayLevel> airplayLevels; // by receiver, only the Server's
    std::string followedDefault; // the endpoint whose parameters are in `wasapi` while it follows the Windows default
    std::wstring lastFile;
    bool adoptOutputSets = false; // from an earlier version: each backend's set still has to reach its device
};

// Settings › Reset (docs/Plan.md §8.2). Playback and output: backend, devices, every backend parameter, conversion,
// decoders, autoplay, playing on from a folder and the DSP chain, whose saved EQ presets stay. Interface: language, close action,
// the tray notice, the first-run guide, the hotkeys, the automatic update check, the online artist pictures, the folder tree, the
// track list columns and Discord. Each keeps the other group.
Settings resetPlayback(const Settings& s);
Settings resetInterface(const Settings& s);

// The selected backend's fixed rate (0: follow the file) and timing.
uint32_t fixedRate(const Settings& s);
// The selected backend's highest PCM rate in Hz and highest DSD rate as a DSD byte frame rate; 0 has no limit, as for
// WASAPI shared mode and AirPlay.
uint32_t maxSampleRate(const Settings& s);
uint32_t maxDsdByteRate(const Settings& s);
const PlaybackTiming& timing(const Settings& s);

std::filesystem::path dataDirectory(); // %LOCALAPPDATA%\CamelPlayer (created on demand)
std::filesystem::path settingsPath();

// Settings a newer CamelPlayer wrote (a higher schema): this version neither reads nor replaces them.
class NewerSettingsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Missing or corrupt file -> defaults (the corrupt file is kept as settings.json.bad). Throws NewerSettingsError.
Settings loadSettings(const std::filesystem::path& path = settingsPath());

enum class SettingsRecovery : uint8_t { None, RestoredLastGood, Reset };
// loadSettings at the program's start: a readable file is also copied to settings.json.last-good, and that copy replaces
// a corrupt file before the defaults would. `recovery` tells which happened. Throws NewerSettingsError.
Settings openSettings(const std::filesystem::path& path, SettingsRecovery& recovery);
// Atomic replace (write temp file, then MoveFileEx).
void saveSettings(const Settings& s, const std::filesystem::path& path = settingsPath());

std::string toJson(const Settings& s);
Settings fromJson(const std::string& json); // throws on malformed JSON

std::optional<audio::SampleType> sampleTypeFromId(std::string_view id); // "16", "24", "24in32", "32", "f32"; "" = auto
const char* sampleTypeId(audio::SampleType t);

} // namespace camel
