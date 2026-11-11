// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/settings/Settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

#pragma comment(lib, "shell32.lib")

namespace camel {

using audio::SampleType;
using nlohmann::json;

const char* backendId(Backend b) {
    switch (b) {
    case Backend::Wasapi:
        return "wasapi";
    case Backend::Asio:
        return "asio";
    case Backend::Ks:
        return "ks";
    case Backend::Network:
        return "network";
    case Backend::AirPlay:
        return "airplay";
    }
    return "wasapi";
}

const char* backendName(Backend b) {
    switch (b) {
    case Backend::Wasapi:
        return "WASAPI";
    case Backend::Asio:
        return "ASIO";
    case Backend::Ks:
        return "Kernel Streaming";
    case Backend::Network:
        return "UPnP";
    case Backend::AirPlay:
        return "AirPlay";
    }
    return "?";
}

bool backendFromId(std::string_view id, Backend& out) {
    for (Backend b : {Backend::Wasapi, Backend::Asio, Backend::Ks, Backend::Network, Backend::AirPlay})
        if (id == backendId(b)) {
            out = b;
            return true;
        }
    return false;
}

const char* ksModeId(KsMode m) {
    switch (m) {
    case KsMode::Auto:
        return "auto";
    case KsMode::WaveRtEvent:
        return "rtevent";
    case KsMode::WaveRtPoll:
        return "rtpoll";
    case KsMode::Standard:
        return "standard";
    }
    return "auto";
}

const char* dsdOutputId(DsdOutput d) {
    return d == DsdOutput::Native ? "native" : d == DsdOutput::Pcm ? "pcm" : "dop";
}

static DsdOutput dsdOutputFromId(std::string_view id, DsdOutput fallback) {
    return id == "native" ? DsdOutput::Native : id == "dop" ? DsdOutput::Dop : id == "pcm" ? DsdOutput::Pcm : fallback;
}

static uint32_t dsdPcmRateFrom(double rate) {
    for (uint32_t r : kDsdPcmRates)
        if (rate == r) return r;
    return 176400;
}

const char* resampleQualityId(ResampleQuality q) {
    return q == ResampleQuality::Quick ? "quick" : q == ResampleQuality::VeryHigh ? "veryhigh" : "high";
}

static ResampleQuality resampleQualityFromId(std::string_view id) {
    return id == "quick" ? ResampleQuality::Quick : id == "veryhigh" ? ResampleQuality::VeryHigh : ResampleQuality::High;
}

static KsMode ksModeFromId(std::string_view id) {
    for (KsMode m : {KsMode::Auto, KsMode::WaveRtEvent, KsMode::WaveRtPoll, KsMode::Standard})
        if (id == ksModeId(m)) return m;
    return KsMode::Auto;
}

const char* sampleTypeId(SampleType t) {
    switch (t) {
    case SampleType::Int16:
        return "16";
    case SampleType::Int24:
        return "24";
    case SampleType::Int24In32:
        return "24in32";
    case SampleType::Int32:
        return "32";
    case SampleType::Float32:
        return "f32";
    case SampleType::Dsd8:
        return ""; // never a fixed container
    }
    return "";
}

std::optional<SampleType> sampleTypeFromId(std::string_view id) {
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32, SampleType::Float32})
        if (id == sampleTypeId(t)) return t;
    return std::nullopt;
}

static const char* periodId(WasapiPeriod p) {
    return p == WasapiPeriod::Minimum ? "minimum" : p == WasapiPeriod::Custom ? "custom" : "default";
}
static WasapiPeriod periodFromId(std::string_view id) {
    return id == "minimum" ? WasapiPeriod::Minimum : id == "custom" ? WasapiPeriod::Custom : WasapiPeriod::Default;
}

const wchar_t* mmcssTaskName(MmcssTask t) {
    return t == MmcssTask::ProAudio ? L"Pro Audio" : t == MmcssTask::Playback ? L"Playback" : nullptr;
}

static const char* mmcssTaskId(MmcssTask t) {
    return t == MmcssTask::Playback ? "playback" : t == MmcssTask::Disabled ? "disabled" : "proaudio";
}
static MmcssTask mmcssTaskFromId(std::string_view id) {
    return id == "playback" ? MmcssTask::Playback : id == "disabled" ? MmcssTask::Disabled : MmcssTask::ProAudio;
}
static const char* mmcssPriorityId(MmcssPriority p) {
    return p == MmcssPriority::Normal ? "normal" : p == MmcssPriority::Critical ? "critical" : "high";
}
static MmcssPriority mmcssPriorityFromId(std::string_view id) {
    return id == "normal" ? MmcssPriority::Normal : id == "critical" ? MmcssPriority::Critical : MmcssPriority::High;
}

static uint32_t fixedRateFrom(double rate) {
    for (uint32_t r : kFixedRates)
        if (rate == r) return r;
    return 0;
}

static uint32_t maxDsdRateFrom(double rate) {
    for (uint32_t r : kMaxDsdRates)
        if (rate == r) return r;
    return 0;
}

static constexpr const char* kEqKindIds[] = {"peaking",  "lowShelf", "highShelf", "lowPass",
                                             "highPass", "bandPass", "notch",     "allPass"};
static constexpr const char* kEqChannelIds[] = {"all", "left", "right"};
static constexpr const char* kCrossfeedPresetIds[] = {"default", "chuMoy", "janMeier", "custom"};
static constexpr const char* kDspNodeIds[] = {"eq", "crossfeed", "convolution"};
static constexpr const char* kReplayGainModeIds[] = {"off", "track", "album", "auto"};
static constexpr const char* kScanSpeedIds[] = {"low", "medium", "high"};

double volumeDb(const VolumeSettings& volume) {
    if (volume.mode != VolumeMode::Software) return 0.0;
    return volume.muted ? -std::numeric_limits<double>::infinity() : volume.db;
}

std::pair<uint32_t, double> crossfeedValues(const CrossfeedSettings& crossfeed) {
    switch (crossfeed.preset) {
    case CrossfeedPreset::ChuMoy:
        return {700, 6.0};
    case CrossfeedPreset::JanMeier:
        return {650, 9.5};
    case CrossfeedPreset::Custom:
        return {crossfeed.cutoffHz, crossfeed.feedDb};
    default:
        return {700, 4.5};
    }
}

std::vector<EqBand> defaultEqBands() {
    return {{EqKind::LowShelf, EqChannels::All, 12, 100.0},
            {EqKind::Peaking, EqChannels::All, 12, 400.0},
            {EqKind::Peaking, EqChannels::All, 12, 1000.0},
            {EqKind::Peaking, EqChannels::All, 12, 4000.0},
            {EqKind::HighShelf, EqChannels::All, 12, 10000.0}};
}

static json eqBandsJson(const std::vector<EqBand>& bands) {
    json out = json::array();
    for (const auto& b : bands)
        out.push_back({{"type", kEqKindIds[int(b.kind) - 1]},
                       {"channels", kEqChannelIds[int(b.channels)]},
                       {"slope", b.slope},
                       {"frequency", b.frequencyHz},
                       {"gain", b.gainDb},
                       {"q", b.q},
                       {"enabled", b.enabled}});
    return out;
}

// Bands that cannot be read are dropped; `fallback` stands in for a missing or unreadable list.
static std::vector<EqBand> eqBandsFrom(const json& list, const std::vector<EqBand>& fallback) {
    if (!list.is_array()) return fallback;
    std::vector<EqBand> bands;
    for (const auto& item : list) {
        if (!item.is_object()) continue;
        EqBand band;
        const auto type = item.value("type", std::string());
        const auto kind = std::ranges::find(kEqKindIds, type);
        if (kind == std::end(kEqKindIds)) continue;
        band.kind = EqKind(kind - std::begin(kEqKindIds) + 1);
        const auto channels = std::ranges::find(kEqChannelIds, item.value("channels", std::string("all")));
        band.channels = channels == std::end(kEqChannelIds) ? EqChannels::All : EqChannels(channels - std::begin(kEqChannelIds));
        const auto number = [&](const char* key, double fallbackValue, double minimum, double maximum) {
            if (!item.contains(key) || !item[key].is_number()) return fallbackValue;
            const double value = item[key].get<double>();
            return std::isfinite(value) && value >= minimum && value <= maximum ? value : fallbackValue;
        };
        const double slope = number("slope", 12, 6, 48);
        band.slope = std::fmod(slope, 6.0) == 0.0 ? uint32_t(slope) : 12;
        band.frequencyHz = number("frequency", 1000.0, 1.0, 1e6);
        band.gainDb = number("gain", 0.0, -60.0, 60.0);
        band.q = number("q", 0.71, 0.01, 100.0);
        band.enabled = !item.contains("enabled") || !item["enabled"].is_boolean() || item["enabled"].get<bool>();
        bands.push_back(band);
    }
    return bands;
}

static json timingJson(const PlaybackTiming& t) {
    return {{"predecodeSeconds", t.predecodeSeconds},
            {"formatChangeSilenceMs", t.formatChangeSilenceMs},
            {"releaseAfterPauseSeconds", t.releaseAfterPauseSeconds}};
}

uint32_t fixedRate(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return s.wasapi.shared ? 0 : s.wasapi.fixedRate;
    case Backend::Asio:
        return s.asio.fixedRate;
    case Backend::Ks:
        return s.ks.fixedRate;
    case Backend::Network:
    case Backend::AirPlay:
        return 0;
    }
    return 0;
}

uint32_t maxSampleRate(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return s.wasapi.shared ? 0 : s.wasapi.maxSampleRate;
    case Backend::Asio:
        return s.asio.maxSampleRate;
    case Backend::Ks:
        return s.ks.maxSampleRate;
    case Backend::Network:
        return s.network.maxSampleRate;
    case Backend::AirPlay:
        return 0;
    }
    return 0;
}

uint32_t maxDsdByteRate(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return s.wasapi.shared ? 0 : dsdByteRate(s.wasapi.maxDsdRate);
    case Backend::Asio:
        return dsdByteRate(s.asio.maxDsdRate);
    case Backend::Ks:
        return dsdByteRate(s.ks.maxDsdRate);
    case Backend::Network:
        return dsdByteRate(s.network.maxDsdRate);
    case Backend::AirPlay:
        return 0;
    }
    return 0;
}

const PlaybackTiming& timing(const Settings& s) {
    return s.backend == Backend::Asio      ? s.asio.timing
           : s.backend == Backend::Ks      ? s.ks.timing
           : s.backend == Backend::Network ? s.network.timing
           : s.backend == Backend::AirPlay ? s.airplay.timing
                                           : s.wasapi.timing;
}

audio::DecoderKind decoderFor(const DecoderByFormat& choices, audio::AudioFormat format) {
    if (format == audio::AudioFormat::Dts) format = audio::AudioFormat::Wav; // DTS-CD is read by the WAVE decoder
    return choices[format].value_or(audio::defaultDecoder(format));
}

static std::string str(const json& o, const char* k, const std::string& def) {
    return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : def;
}

static double num(const json& o, const char* k, double def, double minimum, double maximum) {
    if (!o.contains(k) || !o[k].is_number()) return def;
    const double value = o[k].get<double>();
    return std::isfinite(value) && value >= minimum && value <= maximum ? value : def;
}

static int integer(const json& o, const char* k, int def, int minimum, int maximum) {
    const double value = num(o, k, def, minimum, maximum);
    return std::trunc(value) == value ? int(value) : def;
}

static bool boolean(const json& o, const char* k, bool def) {
    return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def;
}

static void readTiming(const json& o, PlaybackTiming& t) {
    t.predecodeSeconds = uint32_t(integer(o, "predecodeSeconds", 10, 1, 60));
    t.formatChangeSilenceMs = uint32_t(integer(o, "formatChangeSilenceMs", 0, 0, 2000));
    t.releaseAfterPauseSeconds = uint32_t(integer(o, "releaseAfterPauseSeconds", int(t.releaseAfterPauseSeconds), 0, 3600));
}

static const char* volumeModeId(VolumeMode mode) {
    return mode == VolumeMode::Software ? "software" : mode == VolumeMode::Player ? "player" : "fixed";
}

static VolumeMode volumeModeFromId(const std::string& id) {
    return id == "software" ? VolumeMode::Software : id == "player" ? VolumeMode::Player : VolumeMode::Fixed;
}

static json wasapiJson(const WasapiSettings& w) {
    json j = {{"device", w.deviceId},
              {"shareMode", w.shared ? "shared" : "exclusive"},
              {"sharedResampleQuality", resampleQualityId(w.sharedResampleQuality)},
              {"eventDriven", w.eventDriven},
              {"period", periodId(w.period)},
              {"customPeriodMs", w.customPeriodMs},
              {"sampleType", w.sampleType ? sampleTypeId(*w.sampleType) : ""},
              {"dsdOutput", dsdOutputId(w.dsdOutput)},
              {"dsdPcmRate", w.dsdPcmRate},
              {"fixedRate", w.fixedRate},
              {"maxSampleRate", w.maxSampleRate},
              {"maxDsdRate", w.maxDsdRate},
              {"pollBufferMs", w.pollBufferMs},
              {"mmcssTask", mmcssTaskId(w.mmcssTask)},
              {"mmcssPriority", mmcssPriorityId(w.mmcssPriority)}};
    j.update(timingJson(w.timing));
    return j;
}

static void readWasapi(const json& o, WasapiSettings& w) {
    w.deviceId = str(o, "device", "");
    w.shared = str(o, "shareMode", "exclusive") == "shared";
    w.sharedResampleQuality = resampleQualityFromId(str(o, "sharedResampleQuality", "high"));
    w.eventDriven = boolean(o, "eventDriven", true);
    w.period = periodFromId(str(o, "period", "default"));
    w.customPeriodMs = num(o, "customPeriodMs", 10.0, 1.0, 500.0);
    w.sampleType = sampleTypeFromId(str(o, "sampleType", ""));
    w.dsdOutput = dsdOutputFromId(str(o, "dsdOutput", ""), DsdOutput::Dop);
    if (w.dsdOutput == DsdOutput::Native) w.dsdOutput = DsdOutput::Dop; // WASAPI has no native DSD
    w.dsdPcmRate = dsdPcmRateFrom(num(o, "dsdPcmRate", 176400, 0, 1e7));
    w.fixedRate = fixedRateFrom(num(o, "fixedRate", 0, 0, 1e7));
    w.maxSampleRate = fixedRateFrom(num(o, "maxSampleRate", 0, 0, 1e7));
    w.maxDsdRate = maxDsdRateFrom(num(o, "maxDsdRate", 0, 0, 1e4));
    w.pollBufferMs = num(o, "pollBufferMs", 200.0, 20.0, 2000.0);
    w.mmcssTask = mmcssTaskFromId(str(o, "mmcssTask", ""));
    w.mmcssPriority = mmcssPriorityFromId(str(o, "mmcssPriority", ""));
    readTiming(o, w.timing);
}

static json asioJson(const AsioSettings& a) {
    json j = {{"driver", a.driverClsid},
              {"bufferSize", a.bufferSize},
              {"outLeft", a.outLeft},
              {"outRight", a.outRight},
              {"dsdOutput", dsdOutputId(a.dsdOutput)},
              {"dsdPcmRate", a.dsdPcmRate},
              {"fixedRate", a.fixedRate},
              {"maxSampleRate", a.maxSampleRate},
              {"maxDsdRate", a.maxDsdRate}};
    j.update(timingJson(a.timing));
    return j;
}

static void readAsio(const json& o, AsioSettings& a) {
    a.driverClsid = str(o, "driver", "");
    a.bufferSize = integer(o, "bufferSize", 0, 0, 65536);
    a.outLeft = integer(o, "outLeft", 0, 0, 63);
    a.outRight = integer(o, "outRight", 1, 0, 63);
    a.dsdOutput = dsdOutputFromId(str(o, "dsdOutput", ""), DsdOutput::Native);
    a.dsdPcmRate = dsdPcmRateFrom(num(o, "dsdPcmRate", 176400, 0, 1e7));
    a.fixedRate = fixedRateFrom(num(o, "fixedRate", 0, 0, 1e7));
    a.maxSampleRate = fixedRateFrom(num(o, "maxSampleRate", 0, 0, 1e7));
    a.maxDsdRate = maxDsdRateFrom(num(o, "maxDsdRate", 0, 0, 1e4));
    readTiming(o, a.timing);
}

static json ksJson(const KsSettings& k) {
    json j = {{"device", k.deviceId},
              {"mode", ksModeId(k.mode)},
              {"bufferMs", k.bufferMs},
              {"packetCount", k.packetCount},
              {"packetMs", k.packetMs},
              {"sampleType", k.sampleType ? sampleTypeId(*k.sampleType) : ""},
              {"dsdOutput", dsdOutputId(k.dsdOutput)},
              {"dsdPcmRate", k.dsdPcmRate},
              {"fixedRate", k.fixedRate},
              {"maxSampleRate", k.maxSampleRate},
              {"maxDsdRate", k.maxDsdRate},
              {"mmcssTask", mmcssTaskId(k.mmcssTask)},
              {"mmcssPriority", mmcssPriorityId(k.mmcssPriority)}};
    j.update(timingJson(k.timing));
    return j;
}

static void readKs(const json& o, KsSettings& k) {
    k.deviceId = str(o, "device", "");
    k.mode = ksModeFromId(str(o, "mode", "auto"));
    k.bufferMs = num(o, "bufferMs", 20.0, 2.0, 1000.0);
    k.packetCount = integer(o, "packetCount", 4, 2, 16);
    k.packetMs = num(o, "packetMs", 10.0, 1.0, 200.0);
    k.sampleType = sampleTypeFromId(str(o, "sampleType", ""));
    k.dsdOutput = dsdOutputFromId(str(o, "dsdOutput", ""), DsdOutput::Dop);
    if (k.dsdOutput == DsdOutput::Native) k.dsdOutput = DsdOutput::Dop; // Windows defines no KS DSD format
    k.dsdPcmRate = dsdPcmRateFrom(num(o, "dsdPcmRate", 176400, 0, 1e7));
    k.fixedRate = fixedRateFrom(num(o, "fixedRate", 0, 0, 1e7));
    k.maxSampleRate = fixedRateFrom(num(o, "maxSampleRate", 0, 0, 1e7));
    k.maxDsdRate = maxDsdRateFrom(num(o, "maxDsdRate", 0, 0, 1e4));
    k.mmcssTask = mmcssTaskFromId(str(o, "mmcssTask", ""));
    k.mmcssPriority = mmcssPriorityFromId(str(o, "mmcssPriority", ""));
    readTiming(o, k.timing);
}

static json networkJson(const NetworkSettings& n) {
    json j = {{"player", n.udn},      {"maxSampleRate", n.maxSampleRate},      {"maxDsdRate", n.maxDsdRate},
              {"maxBits", n.maxBits}, {"dsdOutput", dsdOutputId(n.dsdOutput)}, {"dsdPcmRate", n.dsdPcmRate},
              {"gapless", n.gapless}};
    j.update(timingJson(n.timing));
    return j;
}

static void readNetwork(const json& o, NetworkSettings& n) {
    n.udn = str(o, "player", "");
    n.maxSampleRate = fixedRateFrom(num(o, "maxSampleRate", 0, 0, 1e7));
    n.maxDsdRate = maxDsdRateFrom(num(o, "maxDsdRate", 0, 0, 1e4));
    n.maxBits = integer(o, "maxBits", 0, 0, 16) == 16 ? 16 : 0;
    n.dsdOutput = str(o, "dsdOutput", "") == dsdOutputId(DsdOutput::Pcm) ? DsdOutput::Pcm : DsdOutput::Native;
    n.dsdPcmRate = dsdPcmRateFrom(num(o, "dsdPcmRate", 176400, 0, 1e7));
    n.gapless = boolean(o, "gapless", true);
    readTiming(o, n.timing);
}

static const char* const kAirPlayChannelIds[] = {"stereo", "left", "right"};

static json airplayJson(const AirPlaySettings& a) {
    json j = {{"receiver", a.deviceId}, {"channels", kAirPlayChannelIds[int(a.channels)]}};
    j.update(timingJson(a.timing));
    return j;
}

static void readAirPlay(const json& o, AirPlaySettings& a) {
    a.deviceId = str(o, "receiver", "");
    const std::string channels = str(o, "channels", "stereo");
    a.channels = channels == "left"    ? AirPlayChannels::Left
                 : channels == "right" ? AirPlayChannels::Right
                                       : AirPlayChannels::Stereo;
    readTiming(o, a.timing);
}

static json outputJson(const SavedOutput& o) {
    json j = {
        {"backend", backendId(o.backend)},         {"id", o.id}, {"name", o.name}, {"alias", o.alias}, {"enabled", o.enabled},
        {"volumeMode", volumeModeId(o.volumeMode)}};
    j["parameters"] = o.backend == Backend::Network   ? networkJson(o.network)
                      : o.backend == Backend::AirPlay ? airplayJson(o.airplay)
                      : o.backend == Backend::Asio    ? asioJson(o.asio)
                      : o.backend == Backend::Ks      ? ksJson(o.ks)
                                                      : wasapiJson(o.wasapi);
    if (!o.speakers.empty()) j["speakers"] = o.speakers;
    return j;
}

static std::optional<SavedOutput> outputFrom(const json& o) {
    SavedOutput saved;
    if (!o.is_object() || !backendFromId(str(o, "backend", ""), saved.backend)) return std::nullopt;
    saved.id = str(o, "id", "");
    saved.name = str(o, "name", "");
    saved.alias = str(o, "alias", "");
    saved.enabled = boolean(o, "enabled", false);
    saved.volumeMode = volumeModeFromId(str(o, "volumeMode", "fixed"));
    if (o.contains("speakers") && o["speakers"].is_array())
        for (const auto& speaker : o["speakers"])
            if (speaker.is_string()) saved.speakers.push_back(speaker.get<std::string>());
    const json& parameters = o.contains("parameters") && o["parameters"].is_object() ? o["parameters"] : json::object();
    if (saved.backend == Backend::Network)
        readNetwork(parameters, saved.network);
    else if (saved.backend == Backend::AirPlay)
        readAirPlay(parameters, saved.airplay);
    else if (saved.backend == Backend::Asio)
        readAsio(parameters, saved.asio);
    else if (saved.backend == Backend::Ks)
        readKs(parameters, saved.ks);
    else
        readWasapi(parameters, saved.wasapi);
    saved.wasapi.deviceId = saved.asio.driverClsid = saved.ks.deviceId = saved.network.udn = saved.airplay.deviceId = saved.id;
    return saved;
}

std::string toJson(const Settings& s) {
    json j;
    j["schema"] = Settings::kSchemaVersion;
    j["backend"] = backendId(s.backend);
    json decoders = json::object();
    for (const auto format : audio::kAllFormats)
        if (const auto decoder = s.decoderByFormat[format]) decoders[audio::formatId(format)] = audio::decoderId(*decoder);
    j["decoderByFormat"] = decoders;
    // A new key: it was "convertWhenUnsupported" while off by default, which is ignored so that conversion starts on
    // for everyone once; versions that know only the old key keep reading theirs.
    j["convertUnsupported"] = s.convertWhenUnsupported;
    j["resampleQuality"] = resampleQualityId(s.resampleQuality);
    j["convertDsdWhenUnsupported"] = s.convertDsdWhenUnsupported;
    j["language"] = s.language;
    j["closeAction"] = s.closeAction == CloseAction::Exit ? "exit" : "tray";
    j["theme"] = s.theme == Theme::Light ? "light" : s.theme == Theme::Dark ? "dark" : "system";
    j["trayNoticeShown"] = s.trayNoticeShown;
    j["firstRunDone"] = s.firstRunDone;
    j["autoplay"] = s.autoplay;
    j["folderPlaysOn"] = s.folderPlaysOn;
    j["checkUpdates"] = s.checkUpdates;
    j["artistPictures"] = s.artistPictures == OnlineLookup::Allowed   ? "allowed"
                          : s.artistPictures == OnlineLookup::Refused ? "refused"
                                                                      : "ask";
    j["folderTree"] = s.folderTree;
    j["scanSpeed"] = kScanSpeedIds[int(s.scanSpeed)];
    j["waveform"] = {{"enabled", s.waveform.enabled},
                     {"anyTime", s.waveform.anyTime},
                     {"from", s.waveform.fromMinute},
                     {"to", s.waveform.toMinute}};
    j["trackColumns"] = {s.trackColumns.title, s.trackColumns.artist, s.trackColumns.album};
    j["discord"] = {{"enabled", s.discord.enabled}, {"covers", s.discord.covers}};
    j["skippedVersion"] = s.skippedVersion;
    j["hotkeys"] = {{"playPause", {s.hotkeys.playPause.modifiers, s.hotkeys.playPause.key}},
                    {"previous", {s.hotkeys.previous.modifiers, s.hotkeys.previous.key}},
                    {"next", {s.hotkeys.next.modifiers, s.hotkeys.next.key}}};
    json presets = json::array();
    for (const auto& p : s.eq.presets) presets.push_back({{"name", p.name}, {"bands", eqBandsJson(p.bands)}});
    j["eq"] = {{"enabled", s.eq.enabled}, {"bands", eqBandsJson(s.eq.bands)}, {"preset", s.eq.preset}, {"presets", presets}};
    j["crossfeed"] = {{"enabled", s.crossfeed.enabled},
                      {"preset", kCrossfeedPresetIds[int(s.crossfeed.preset)]},
                      {"cutoff", s.crossfeed.cutoffHz},
                      {"feed", s.crossfeed.feedDb}};
    j["convolution"] = {{"enabled", s.convolution.enabled}, {"file", s.convolution.file}};
    j["volume"] = {{"mode", volumeModeId(s.volume.mode)}, {"db", s.volume.db}, {"muted", s.volume.muted}};
    j["replayGain"] = {{"mode", kReplayGainModeIds[int(s.replayGain.mode)]},
                       {"preamp", s.replayGain.preampDb},
                       {"noValuePreamp", s.replayGain.noValuePreampDb},
                       {"preventClipping", s.replayGain.preventClipping}};
    j["crossfade"] = {{"enabled", s.crossfade.enabled}, {"seconds", s.crossfade.seconds}};
    j["dspOrder"] = json::array();
    for (const auto node : s.dspOrder) j["dspOrder"].push_back(kDspNodeIds[int(node) - 1]);
    j["lastFile"] = toUtf8(s.lastFile);
    j["wasapi"] = wasapiJson(s.wasapi);
    j["asio"] = asioJson(s.asio);
    j["ks"] = ksJson(s.ks);
    j["network"] = networkJson(s.network);
    j["airplay"] = airplayJson(s.airplay);
    j["outputs"] = json::array();
    for (const auto& output : s.outputs) j["outputs"].push_back(outputJson(output));
    if (!s.airplayGroup.empty()) j["airplayGroup"] = s.airplayGroup;
    if (!s.airplayLevels.empty()) {
        j["airplayLevels"] = json::object();
        for (const auto& [id, level] : s.airplayLevels) j["airplayLevels"][id] = {{"level", level.level}, {"muted", level.muted}};
    }
    j["followedDefault"] = s.followedDefault;
    if (s.adoptOutputSets) j["adoptOutputSets"] = true;
    // Any text that is not UTF-8 is replaced rather than stopping every later save.
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

Settings fromJson(const std::string& text) {
    json j = json::parse(text);
    if (!j.is_object()) throw std::invalid_argument("settings must be a JSON object");
    Settings s;
    if (j.contains("schema") && !j["schema"].is_number_integer()) throw std::invalid_argument("invalid settings schema");
    const int schema = j.contains("schema") ? j["schema"].get<int>() : 0;
    if (schema < 0 || schema > Settings::kSchemaVersion) throw std::invalid_argument("unsupported settings schema");
    backendFromId(str(j, "backend", "wasapi"), s.backend);
    // A choice survives only when that decoder is integrated for that format.
    auto choose = [&](std::string_view formatId, std::string_view decoderId) {
        audio::AudioFormat format{};
        audio::DecoderKind decoder = audio::DecoderKind::Sndfile;
        if (audio::formatFromId(formatId, format) && audio::decoderFromId(decoderId, decoder) && audio::supports(format, decoder))
            s.decoderByFormat[format] = decoder;
    };
    if (schema < 3) {
        if (j.contains("decoder") && j["decoder"].is_string()) choose("flac", j["decoder"].get<std::string>());
    } else if (j.contains("decoderByFormat") && j["decoderByFormat"].is_object()) {
        for (const auto& [formatId, decoderId] : j["decoderByFormat"].items())
            if (decoderId.is_string()) choose(formatId, decoderId.get<std::string>());
    }
    // "pcmBitsByFormat", the 16- or 24-bit choice for AAC and MP3 of earlier versions, is no longer read: a lossy stream
    // decodes to 24-bit PCM.
    s.convertWhenUnsupported = boolean(j, "convertUnsupported", true);
    s.resampleQuality = resampleQualityFromId(str(j, "resampleQuality", "high"));
    s.convertDsdWhenUnsupported = boolean(j, "convertDsdWhenUnsupported", false);
    s.language = str(j, "language", "");
    s.closeAction = str(j, "closeAction", "tray") == "exit" ? CloseAction::Exit : CloseAction::Tray;
    const auto theme = str(j, "theme", "system");
    s.theme = theme == "light" ? Theme::Light : theme == "dark" ? Theme::Dark : Theme::System;
    s.trayNoticeShown = boolean(j, "trayNoticeShown", false);
    s.firstRunDone = boolean(j, "firstRunDone", false);
    s.autoplay = boolean(j, "autoplay", true);
    s.folderPlaysOn = boolean(j, "folderPlaysOn", false);
    s.checkUpdates = boolean(j, "checkUpdates", true);
    const auto pictures = str(j, "artistPictures", "ask");
    s.artistPictures = pictures == "allowed"   ? OnlineLookup::Allowed
                       : pictures == "refused" ? OnlineLookup::Refused
                                               : OnlineLookup::Ask;
    s.folderTree = boolean(j, "folderTree", false);
    const auto speed = std::ranges::find(kScanSpeedIds, str(j, "scanSpeed", "low"));
    s.scanSpeed = speed == std::end(kScanSpeedIds) ? ScanSpeed::Low : ScanSpeed(speed - std::begin(kScanSpeedIds));
    if (const auto w = j.find("waveform"); w != j.end() && w->is_object()) {
        s.waveform.enabled = boolean(*w, "enabled", true);
        s.waveform.anyTime = boolean(*w, "anyTime", false);
        const auto minute = [&](const char* key, int fallback) {
            const auto value = w->find(key);
            return value != w->end() && value->is_number_integer() ? std::clamp(value->get<int>(), 0, 1439) : fallback;
        };
        s.waveform.fromMinute = minute("from", 60);
        s.waveform.toMinute = minute("to", 300);
    }
    if (const auto columns = j.find("trackColumns");
        columns != j.end() && columns->is_array() && columns->size() == 3 &&
        std::all_of(columns->begin(), columns->end(),
                    [](const json& c) { return c.is_number() && std::isfinite(c.get<double>()) && c.get<double>() > 0; }))
        s.trackColumns = {(*columns)[0].get<double>(), (*columns)[1].get<double>(), (*columns)[2].get<double>()};
    if (j.contains("discord") && j["discord"].is_object()) {
        s.discord.enabled = boolean(j["discord"], "enabled", false);
        s.discord.covers = boolean(j["discord"], "covers", false);
    }
    s.skippedVersion = str(j, "skippedVersion", "");
    if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
        const json& h = j["hotkeys"];
        const auto hotkey = [&](const char* name, Hotkey fallback) {
            if (!h.contains(name)) return fallback;
            const json& v = h[name];
            if (!v.is_array() || v.size() != 2 || !v[0].is_number_unsigned() || !v[1].is_number_unsigned() ||
                v[0].get<uint32_t>() > 15 || v[1].get<uint32_t>() > 0xFE)
                return fallback;
            return Hotkey{v[0].get<uint32_t>(), v[1].get<uint32_t>()};
        };
        const HotkeySettings defaults;
        s.hotkeys.playPause = hotkey("playPause", defaults.playPause);
        s.hotkeys.previous = hotkey("previous", defaults.previous);
        s.hotkeys.next = hotkey("next", defaults.next);
    }
    if (j.contains("eq") && j["eq"].is_object()) {
        const json& e = j["eq"];
        s.eq.enabled = boolean(e, "enabled", false);
        s.eq.bands = eqBandsFrom(e.contains("bands") ? e["bands"] : json(), defaultEqBands());
        s.eq.preset = str(e, "preset", "");
        if (e.contains("presets") && e["presets"].is_array())
            for (const auto& p : e["presets"])
                if (p.is_object() && p.contains("name") && p["name"].is_string() && !p["name"].get<std::string>().empty())
                    s.eq.presets.push_back(
                        {p["name"].get<std::string>(), eqBandsFrom(p.contains("bands") ? p["bands"] : json(), {})});
    }
    if (j.contains("crossfeed") && j["crossfeed"].is_object()) {
        const json& c = j["crossfeed"];
        const CrossfeedSettings defaults;
        s.crossfeed.enabled = boolean(c, "enabled", false);
        const auto preset = std::ranges::find(kCrossfeedPresetIds, str(c, "preset", "default"));
        s.crossfeed.preset =
            preset == std::end(kCrossfeedPresetIds) ? defaults.preset : CrossfeedPreset(preset - std::begin(kCrossfeedPresetIds));
        if (c.contains("cutoff") && c["cutoff"].is_number_unsigned() &&
            c["cutoff"].get<uint32_t>() >= CrossfeedSettings::kMinCutoffHz &&
            c["cutoff"].get<uint32_t>() <= CrossfeedSettings::kMaxCutoffHz)
            s.crossfeed.cutoffHz = c["cutoff"].get<uint32_t>();
        if (c.contains("feed") && c["feed"].is_number() && c["feed"].get<double>() >= CrossfeedSettings::kMinFeedDb &&
            c["feed"].get<double>() <= CrossfeedSettings::kMaxFeedDb)
            s.crossfeed.feedDb = c["feed"].get<double>();
    }
    if (j.contains("convolution") && j["convolution"].is_object()) {
        s.convolution.enabled = boolean(j["convolution"], "enabled", false);
        s.convolution.file = str(j["convolution"], "file", "");
    }
    const auto bounded = [](const json& o, const char* k, double def, double low, double high) {
        return o.contains(k) && o[k].is_number() && o[k].get<double>() >= low && o[k].get<double>() <= high ? o[k].get<double>()
                                                                                                            : def;
    };
    if (j.contains("volume") && j["volume"].is_object()) {
        const json& v = j["volume"];
        s.volume.mode = volumeModeFromId(str(v, "mode", "fixed"));
        s.volume.db = bounded(v, "db", 0.0, VolumeSettings::kMinDb, 0.0);
        s.volume.muted = boolean(v, "muted", false);
    }
    if (j.contains("replayGain") && j["replayGain"].is_object()) {
        const json& r = j["replayGain"];
        const auto mode = std::ranges::find(kReplayGainModeIds, str(r, "mode", "off"));
        s.replayGain.mode =
            mode == std::end(kReplayGainModeIds) ? ReplayGainMode::Off : ReplayGainMode(mode - std::begin(kReplayGainModeIds));
        s.replayGain.preampDb = bounded(r, "preamp", 0.0, ReplayGainSettings::kMinPreampDb, ReplayGainSettings::kMaxPreampDb);
        s.replayGain.noValuePreampDb =
            bounded(r, "noValuePreamp", 0.0, ReplayGainSettings::kMinPreampDb, ReplayGainSettings::kMaxPreampDb);
        s.replayGain.preventClipping = boolean(r, "preventClipping", true);
    }
    if (j.contains("crossfade") && j["crossfade"].is_object()) {
        const json& c = j["crossfade"];
        s.crossfade.enabled = boolean(c, "enabled", false);
        const double seconds = bounded(c, "seconds", 3.0, CrossfadeSettings::kMinSeconds, CrossfadeSettings::kMaxSeconds);
        s.crossfade.seconds = std::round(seconds * 2.0) / 2.0;
    }
    if (j.contains("dspOrder") && j["dspOrder"].is_array() && j["dspOrder"].size() == 3) {
        audio::DspOrder order = audio::kDefaultDspOrder;
        bool known = true;
        for (size_t i = 0; i < 3 && known; ++i) {
            const auto& item = j["dspOrder"][i];
            const auto id = std::ranges::find(kDspNodeIds, item.is_string() ? item.get<std::string>() : std::string());
            known = id != std::end(kDspNodeIds);
            if (known) order[i] = audio::DspNode(id - std::begin(kDspNodeIds) + 1);
        }
        auto sorted = order;
        std::ranges::sort(sorted);
        if (known && sorted == audio::kDefaultDspOrder) s.dspOrder = order;
    }
    s.lastFile = toWide(str(j, "lastFile", ""));
    if (j.contains("wasapi") && j["wasapi"].is_object()) readWasapi(j["wasapi"], s.wasapi);
    if (j.contains("asio") && j["asio"].is_object()) readAsio(j["asio"], s.asio);
    if (j.contains("ks") && j["ks"].is_object()) readKs(j["ks"], s.ks);
    if (j.contains("network") && j["network"].is_object()) readNetwork(j["network"], s.network);
    if (j.contains("airplay") && j["airplay"].is_object()) readAirPlay(j["airplay"], s.airplay);
    if (j.contains("outputs") && j["outputs"].is_array())
        for (const auto& item : j["outputs"])
            if (auto output = outputFrom(item)) s.outputs.push_back(std::move(*output));
    if (j.contains("airplayGroup") && j["airplayGroup"].is_array())
        for (const auto& id : j["airplayGroup"])
            if (id.is_string() && std::ranges::find(s.airplayGroup, id.get<std::string>()) == s.airplayGroup.end())
                s.airplayGroup.push_back(id.get<std::string>());
    if (j.contains("airplayLevels") && j["airplayLevels"].is_object())
        for (const auto& [id, level] : j["airplayLevels"].items())
            if (level.is_object())
                s.airplayLevels[id] = {uint32_t(integer(level, "level", 33, 0, 100)), boolean(level, "muted", false)};
    s.followedDefault = str(j, "followedDefault", "");
    s.adoptOutputSets = schema < 4 || boolean(j, "adoptOutputSets", false);
    return s;
}

std::filesystem::path dataDirectory() {
    PWSTR base = nullptr;
    std::filesystem::path dir;
    // Measurements (docs/Plan.md §12.1) run against a separate data folder so the user's library stays untouched.
    std::wstring measured(MAX_PATH, L'\0');
    measured.resize(GetEnvironmentVariableW(L"CAMELPLAYER_DATA", measured.data(), DWORD(measured.size())));
    if (!measured.empty() && measured.size() < MAX_PATH)
        dir = measured;
    else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base)))
        dir = std::filesystem::path(base) / L"CamelPlayer";
    CoTaskMemFree(base);
    std::error_code ec;
    if (dir.empty()) return {}; // never fall back to the working directory, which may be the program folder
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec) ? dir : std::filesystem::path{};
}

Settings resetPlayback(const Settings& s) {
    Settings reset;
    reset.language = s.language;
    reset.closeAction = s.closeAction;
    reset.theme = s.theme;
    reset.trayNoticeShown = s.trayNoticeShown;
    reset.firstRunDone = s.firstRunDone;
    reset.hotkeys = s.hotkeys;
    reset.checkUpdates = s.checkUpdates;
    reset.artistPictures = s.artistPictures;
    reset.folderTree = s.folderTree;
    reset.scanSpeed = s.scanSpeed;
    reset.waveform = s.waveform;
    reset.trackColumns = s.trackColumns;
    reset.discord = s.discord;
    reset.skippedVersion = s.skippedVersion;
    reset.eq.presets = s.eq.presets;
    reset.lastFile = s.lastFile;
    return reset;
}

Settings resetInterface(const Settings& s) {
    const Settings defaults;
    Settings reset = s;
    reset.language = defaults.language;
    reset.closeAction = defaults.closeAction;
    reset.theme = defaults.theme;
    reset.trayNoticeShown = defaults.trayNoticeShown;
    reset.firstRunDone = defaults.firstRunDone;
    reset.hotkeys = defaults.hotkeys;
    reset.checkUpdates = defaults.checkUpdates;
    reset.artistPictures = defaults.artistPictures;
    reset.folderTree = defaults.folderTree;
    reset.waveform.enabled = defaults.waveform.enabled;
    reset.trackColumns = defaults.trackColumns;
    reset.discord = defaults.discord;
    return reset;
}

bool inAnalysisWindow(const WaveformSettings& settings, int minute) {
    if (settings.anyTime) return true;
    if (settings.fromMinute == settings.toMinute) return false;
    if (settings.fromMinute < settings.toMinute) return minute >= settings.fromMinute && minute < settings.toMinute;
    return minute >= settings.fromMinute || minute < settings.toMinute;
}

std::filesystem::path settingsPath() {
    return dataDirectory() / L"settings.json";
}

namespace {

constexpr int kOpenAttempts = 10; // 200 ms apart: a backup or antivirus program's hold is often that short

// Nothing for a missing file; throws NewerSettingsError, SettingsUnavailableError for a file that exists and still
// cannot be opened after a moment, or another exception for a corrupt file.
std::optional<Settings> readSettings(const std::filesystem::path& path) {
    std::ifstream in;
    for (int attempt = 0;; ++attempt) {
        in.open(path, std::ios::binary);
        if (in) break;
        std::error_code error;
        if (std::filesystem::status(path, error).type() == std::filesystem::file_type::not_found) return std::nullopt;
        if (attempt == kOpenAttempts)
            throw SettingsUnavailableError(toUtf8(path.filename().wstring()) + " cannot be opened, and was left as it is");
        in.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    const auto contents = ss.str();
    const auto parsed = json::parse(contents);
    if (parsed.is_object() && parsed.contains("schema") && parsed["schema"].is_number_integer() &&
        parsed["schema"].get<int>() > Settings::kSchemaVersion)
        throw NewerSettingsError("settings.json has schema " + std::to_string(parsed["schema"].get<int>()) +
                                 ", newer than this program (" + std::to_string(Settings::kSchemaVersion) + ")");
    Settings settings = fromJson(contents);
    const int schema = parsed.value("schema", 0);
    if (schema < Settings::kSchemaVersion) {
        try {
            saveSettings(settings, path);
        } catch (const std::exception& e) {
            log::warn("settings: loaded schema %d but could not persist migration (%s)", schema, e.what());
        }
    }
    return settings;
}

void keepAsBad(const std::filesystem::path& path, const std::exception& e) {
    log::warn("settings: corrupt file kept as settings.json.bad (%s)", e.what());
    std::error_code ec;
    std::filesystem::rename(path, std::filesystem::path(path).concat(L".bad"), ec);
}

} // namespace

Settings loadSettings(const std::filesystem::path& path) {
    try {
        return readSettings(path).value_or(Settings{});
    } catch (const NewerSettingsError&) {
        throw;
    } catch (const SettingsUnavailableError&) {
        throw;
    } catch (const std::exception& e) {
        keepAsBad(path, e);
        return {};
    }
}

Settings openSettings(const std::filesystem::path& path, SettingsRecovery& recovery) {
    recovery = SettingsRecovery::None;
    const auto lastGood = std::filesystem::path(path).concat(L".last-good");
    std::error_code ec;
    try {
        const auto settings = readSettings(path);
        if (settings) std::filesystem::copy_file(path, lastGood, std::filesystem::copy_options::overwrite_existing, ec);
        return settings.value_or(Settings{});
    } catch (const NewerSettingsError&) {
        throw;
    } catch (const SettingsUnavailableError&) {
        throw;
    } catch (const std::exception& e) {
        keepAsBad(path, e);
    }
    try {
        if (const auto settings = readSettings(lastGood)) {
            saveSettings(*settings, path);
            recovery = SettingsRecovery::RestoredLastGood;
            log::warn("settings: restored settings.json.last-good");
            return *settings;
        }
    } catch (const NewerSettingsError&) {
        throw;
    } catch (const SettingsUnavailableError&) {
        throw;
    } catch (const std::exception& e) {
        log::warn("settings: settings.json.last-good is unusable too (%s)", e.what());
    }
    recovery = SettingsRecovery::Reset;
    return {};
}

void saveSettings(const Settings& s, const std::filesystem::path& path) {
    auto tmp = std::filesystem::path(path).concat(L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << toJson(s);
        out.close();
        if (!out) throw std::runtime_error("cannot write settings");
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot replace settings file");
}

} // namespace camel
