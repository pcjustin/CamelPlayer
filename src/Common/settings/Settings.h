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

enum class Backend : uint8_t { Wasapi, Asio, Ks, Network, AirPlay, HQPlayer };
const char* backendId(Backend b); // "wasapi" / "asio" / "ks" / "network" / "airplay" / "hqplayer"
const char* backendName(Backend b);
inline bool isNetworkPlayer(Backend b) {
    return b == Backend::Network || b == Backend::AirPlay;
}
// Played as a stream another device fetches: a network player or HQPlayer.
inline bool isStreamed(Backend b) {
    return isNetworkPlayer(b) || b == Backend::HQPlayer;
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

// An output's conversion filter (docs/Plan.md §6): soxr's quality, phase and passband, and the order of the modulator
// DSD upsampling modulates with. Soxr's quick recipe is a cubic interpolator, which no phase or passband changes.
enum class FilterPhase : uint8_t { Linear, Intermediate, Minimum };
enum class FilterPassband : uint8_t { Standard, Steep, Gentle };
constexpr uint32_t kModulatorOrders[] = {5, 7};
struct ResampleFilter {
    ResampleQuality quality = ResampleQuality::High;
    FilterPhase phase = FilterPhase::Linear;
    FilterPassband passband = FilterPassband::Standard;
    uint32_t modulatorOrder = 7;
    bool operator==(const ResampleFilter&) const = default;
};

// An output's DSD upsampling (docs/Plan.md §6): off, the highest DSD rate it takes, the source's rate times a multiple,
// or a set rate (DSD128 or DSD256, as a multiple of 44.1 kHz). With `pcm`, PCM sources are sent as DSD too, at the
// highest or the set rate, and by a multiple at the set rate. Every source is then decimated to PCM where it is DSD,
// processed, resampled to 352.8 kHz and modulated again.
enum class DsdUpsampling : uint8_t { Off, Highest, Multiple, Set };
constexpr uint32_t kDsdUpsamplingMultiples[] = {2, 4};
constexpr uint32_t kDsdSetRates[] = {128, 256};
struct DsdUpsamplingSettings {
    DsdUpsampling mode = DsdUpsampling::Off;
    uint32_t multiple = 2;
    uint32_t rate = 128;
    bool pcm = false;
    bool operator==(const DsdUpsamplingSettings&) const = default;
};

// An output's sample-rate choice besides following the file and its set rate (docs/Plan.md §6): the highest rate of
// the source's family, the source's rate times a multiple, or the highest rate of either family the output takes.
enum class Upsampling : uint8_t { Off, IntegerHighest, IntegerMultiple, AnyHighest };
constexpr uint32_t kUpsamplingMultiples[] = {2, 4, 8};

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
    // Exclusive mode only; choosing a rate, or upsampling, is the permission to resample PCM (raw DSD is never
    // resampled). A set rate wins over upsampling.
    uint32_t fixedRate = 0;
    Upsampling upsampling = Upsampling::Off;
    uint32_t upsamplingMultiple = 2;
    ResampleFilter filter;
    DsdUpsamplingSettings dsdUpsampling; // exclusive mode only
    // DSD sources through the DSP chain, ReplayGain and the software volume while any of them changes the signal,
    // modulated again at their own rate; off sends raw DSD untouched. On anyway with DSD upsampling.
    bool dspOnDsd = false;
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
    uint32_t fixedRate = 0; // as for WASAPI
    Upsampling upsampling = Upsampling::Off;
    uint32_t upsamplingMultiple = 2;
    ResampleFilter filter;
    DsdUpsamplingSettings dsdUpsampling;
    bool dspOnDsd = false; // as for WASAPI
    uint32_t maxSampleRate = 0;
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
    uint32_t fixedRate = 0; // as for WASAPI
    Upsampling upsampling = Upsampling::Off;
    uint32_t upsamplingMultiple = 2;
    ResampleFilter filter;
    DsdUpsamplingSettings dsdUpsampling;
    bool dspOnDsd = false; // as for WASAPI
    uint32_t maxSampleRate = 0;
    uint32_t maxDsdRate = 0;
    MmcssTask mmcssTask = MmcssTask::ProAudio;
    MmcssPriority mmcssPriority = MmcssPriority::High;
    PlaybackTiming timing;
    bool operator==(const KsSettings&) const = default;
};

// A UPnP AV or OpenHome player (docs/Plan.md §5.10), kept by its UDN and never by its address.
struct NetworkSettings {
    std::string udn;
    uint32_t fixedRate = 0; // as for WASAPI
    Upsampling upsampling = Upsampling::Off;
    uint32_t upsamplingMultiple = 2;
    ResampleFilter filter;
    DsdUpsamplingSettings dsdUpsampling;     // sent as DSF
    bool dspOnDsd = false;                   // as for WASAPI, sent as DSF
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

// HQPlayer Desktop or Embedded as the output (docs/Plan.md §5.11): it processes and plays, its own settings staying in
// HQPlayer.
struct HQPlayerSettings {
    std::string address;                     // host:port, as added or found
    DsdOutput dsdOutput = DsdOutput::Native; // Native is DSF; Pcm is the permission to convert
    uint32_t dsdPcmRate = 176400;
    PlaybackTiming timing{.releaseAfterPauseSeconds = 0};
    bool operator==(const HQPlayerSettings&) const = default;
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
// The window's colours: the Windows app mode, or always light or dark; black is dark with every surface pure black.
enum class Theme : uint8_t { System, Light, Dark, Black };
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
    // The ranges settings.json keeps; a value outside is not read back.
    static constexpr double kMinFrequencyHz = 1.0, kMaxFrequencyHz = 1e6;
    static constexpr double kMinGainDb = -60.0, kMaxGainDb = 60.0;
    static constexpr double kMinQ = 0.01, kMaxQ = 100.0;
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
// while it is on, at the scan speed.
enum class ReplayGainMode : uint8_t { Off, Track, Album, Auto };

struct ReplayGainSettings {
    static constexpr double kMinPreampDb = -15.0, kMaxPreampDb = 15.0;
    ReplayGainMode mode = ReplayGainMode::Off;
    double preampDb = 0.0;        // tracks with a value
    double noValuePreampDb = 0.0; // tracks without one
    bool preventClipping = true;
    bool operator==(const ReplayGainSettings&) const = default;
};

// How much of the network, the storage and this computer background library work takes: scans, covers and thumbnails,
// fingerprints and loudness analysis. Low uses one worker of each kind, Medium half of High's, High as many as each
// kind can use; a scan the user starts, and the work following it, runs at High whatever this is.
enum class ScanSpeed : uint8_t { Low, Medium, High };

// The waveform in the playback bar (docs/Task-breakdown.md M6.6-R07), on by default. The background analysis that makes
// it for the whole library runs only between `fromMinute` and `toMinute` of the day (01:00 to 05:00 by default; a span
// past midnight when `fromMinute` is the later), or at any time; a playing track's is made at once.
struct WaveformSettings {
    bool enabled = true;
    bool anyTime = false;
    int fromMinute = 60;
    int toMinute = 300;
    bool operator==(const WaveformSettings&) const = default;
};
// Whether the background analysis may run at `minute` of the day.
bool inAnalysisWindow(const WaveformSettings& settings, int minute);

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

enum class RepeatMode : uint8_t { Off, One, All };

// What a zone keeps of its own besides its queue and DSP (docs/Plan.md §5.12): its queue's shuffle and repeat, crossfade
// and autoplay.
struct ZoneChoices {
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    CrossfadeSettings crossfade;
    bool autoplay = true;
    bool operator==(const ZoneChoices&) const = default;
};

// An output's own DSP: EQ, crossfeed, convolution, their order and ReplayGain, all off for an output new to it. The
// EQ presets are not part of it; every output shares them.
struct OutputDsp {
    EqSettings eq; // without presets
    CrossfeedSettings crossfeed;
    ConvolutionSettings convolution;
    audio::DspOrder dspOrder = audio::kDefaultDspOrder;
    ReplayGainSettings replayGain;
    bool operator==(const OutputDsp&) const = default;
};

// An output device the user enabled, named or set up, kept by stable identity (docs/Plan.md §6). Only `backend`'s
// parameters are used; those of the output in use, its DSP among them, live in Settings and are copied here as they
// change.
struct SavedOutput {
    Backend backend = Backend::Wasapi;
    std::string id;       // as in the backend's settings; empty on WASAPI is "Windows default device", without parameters
    std::string name;     // as the device last called itself, for when it is not connected
    std::string model;    // a network player's maker and model as last seen, to know it again under a new UDN
    std::string alias;    // the user's name for it; empty shows `name`
    bool enabled = false; // listed in the output menu
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    NetworkSettings network;
    AirPlaySettings airplay;
    HQPlayerSettings hqplayer;
    VolumeMode volumeMode = VolumeMode::Fixed;
    OutputDsp dsp;
    ZoneChoices zone;
    // An AirPlay stereo pair's speakers by their own device IDs, which stay when pairing it again gives it a new ID.
    std::vector<std::string> speakers;
    // An AirPlay receiver's group, itself first, as Settings::airplayGroup holds the one in use's; empty while alone.
    std::vector<std::string> airplayGroup;
    bool operator==(const SavedOutput&) const = default;
};

struct Settings {
    static constexpr int kSchemaVersion = 5;

    Backend backend = Backend::Wasapi;
    DecoderByFormat decoderByFormat;
    // "Convert when unsupported" (docs/Plan.md §2, §5.3): resample, downmix, reduce depth or decode DTS instead of
    // skipping. On by default; the signal path then shows the conversion, and turning it off plays bit-perfect only.
    bool convertWhenUnsupported = true;
    // The quality of outputs without a filter of their own (AirPlay), and the one each filter started from.
    ResampleQuality resampleQuality = ResampleQuality::High;
    // "Convert DSD when unsupported" (docs/Plan.md §5.3 item 4): DSD to PCM after native/DoP failed. Independent of
    // convertWhenUnsupported, which still governs rates, downmix and depth of the PCM it produces.
    bool convertDsdWhenUnsupported = false;
    WasapiSettings wasapi;
    AsioSettings asio;
    KsSettings ks;
    NetworkSettings network;
    AirPlaySettings airplay;
    HQPlayerSettings hqplayer;
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
    ScanSpeed scanSpeed = ScanSpeed::Low;
    WaveformSettings waveform;
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
    // The output in use's queue: only the Server sets them, from the queue commands.
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    std::vector<SavedOutput> outputs;
    // The AirPlay receivers playing together, the one in `airplay` among them; kept when another output is chosen, so
    // choosing any of them again brings the group back.
    std::vector<std::string> airplayGroup;
    std::map<std::string, AirPlayLevel> airplayLevels; // by receiver, only the Server's
    std::string followedDefault; // the endpoint whose parameters are in `wasapi` while it follows the Windows default
    std::wstring lastFile;
    bool adoptOutputSets = false; // from an earlier version: each backend's set still has to reach its device
    // Read from a version before zones: the saved queue's shuffle and repeat still have to become every zone's. Never
    // written, as the start that reads them restores the queue.
    bool adoptQueueChoices = false;
};

// The DSP of the output in use, and putting another in its place, the EQ presets staying.
OutputDsp outputDsp(const Settings& s);
void setOutputDsp(Settings& s, const OutputDsp& dsp);
// The same for its zone's choices.
ZoneChoices zoneChoices(const Settings& s);
void setZoneChoices(Settings& s, const ZoneChoices& choices);

// Settings › Reset (docs/Plan.md §8.2). Playback and output: backend, devices, every backend parameter, conversion,
// decoders, autoplay, playing on from a folder, the library scan's speed and the DSP chain, whose saved EQ presets stay.
// Interface: language, close action, the tray notice, the first-run guide, the hotkeys, the automatic update check, the
// online artist pictures, the folder tree, the waveform and the hours of its analysis, the track list columns and Discord.
// Each keeps the other group, and both keep the release a user skipped and the last file opened.
Settings resetPlayback(const Settings& s);
Settings resetInterface(const Settings& s);

// The selected backend's fixed rate (0: follow the file) and timing.
uint32_t fixedRate(const Settings& s);
// The selected backend's upsampling and its multiple; Off for WASAPI shared mode and AirPlay, whose rate is set for
// them.
Upsampling upsampling(const Settings& s);
uint32_t upsamplingMultiple(const Settings& s);
// The filter the selected backend resamples with: its own, the shared mode's quality, or AirPlay's global quality.
ResampleFilter resampleFilter(const Settings& s);
// The selected backend's DSD upsampling; off for an output that sends no DSD (WASAPI shared mode, AirPlay, HQPlayer)
// or converts it to PCM.
DsdUpsamplingSettings dsdUpsampling(const Settings& s);
// Whether the selected backend sends DSD sources through the DSP while it acts; false where dsdUpsampling is off for
// the output's sake.
bool dspOnDsd(const Settings& s);
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

// A settings.json that exists but cannot be read, held by another program or refused by its permissions: it is left
// as it is, never taken for a first run.
class SettingsUnavailableError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Missing or corrupt file -> defaults (the corrupt file is kept as settings.json.bad). Throws NewerSettingsError,
// and SettingsUnavailableError once a file that cannot be opened or read has been tried again for a moment.
Settings loadSettings(const std::filesystem::path& path = settingsPath());
// The settings as a tool such as camelcli reads them, beside a player that may be running: never written, migrated or
// renamed; the defaults for a missing or unreadable file. Throws as loadSettings does for a newer or held file.
Settings readSettingsForTool(const std::filesystem::path& path);

enum class SettingsRecovery : uint8_t { None, RestoredLastGood, Reset };
// loadSettings at the program's start: a readable file is also copied to settings.json.last-good, and that copy replaces
// a corrupt file before the defaults would. `recovery` tells which happened. Throws as loadSettings does.
Settings openSettings(const std::filesystem::path& path, SettingsRecovery& recovery);
// Atomic replace (write temp file, then MoveFileEx).
void saveSettings(const Settings& s, const std::filesystem::path& path = settingsPath());

std::string toJson(const Settings& s);
Settings fromJson(const std::string& json); // throws on malformed JSON

std::optional<audio::SampleType> sampleTypeFromId(std::string_view id); // "16", "24", "24in32", "32", "f32"; "" = auto
const char* sampleTypeId(audio::SampleType t);

} // namespace camel
