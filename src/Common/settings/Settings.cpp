// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/settings/Settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>

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
    }
    return "?";
}

bool backendFromId(std::string_view id, Backend& out) {
    for (Backend b : {Backend::Wasapi, Backend::Asio, Backend::Ks})
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

static constexpr const char* kEqKindIds[] = {"peaking",  "lowShelf", "highShelf", "lowPass",
                                             "highPass", "bandPass", "notch",     "allPass"};
static constexpr const char* kEqChannelIds[] = {"all", "left", "right"};
static constexpr const char* kCrossfeedPresetIds[] = {"default", "chuMoy", "janMeier", "custom"};
static constexpr const char* kDspNodeIds[] = {"eq", "crossfeed", "convolution"};

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
    }
    return 0;
}

const PlaybackTiming& timing(const Settings& s) {
    return s.backend == Backend::Asio ? s.asio.timing : s.backend == Backend::Ks ? s.ks.timing : s.wasapi.timing;
}

audio::DecoderKind decoderFor(const DecoderByFormat& choices, audio::AudioFormat format) {
    return choices[format].value_or(audio::defaultDecoder(format));
}

uint32_t pcmBitsFor(const PcmBitsByFormat& choices, audio::AudioFormat format) {
    return choices[format].value_or(24);
}

std::string toJson(const Settings& s) {
    json j;
    j["schema"] = Settings::kSchemaVersion;
    j["backend"] = backendId(s.backend);
    json decoders = json::object();
    for (const auto format : audio::kAllFormats)
        if (const auto decoder = s.decoderByFormat[format]) decoders[audio::formatId(format)] = audio::decoderId(*decoder);
    j["decoderByFormat"] = decoders;
    json pcmBits = json::object();
    for (const auto format : audio::kAllFormats)
        if (const auto bits = s.pcmBitsByFormat[format]) pcmBits[audio::formatId(format)] = *bits;
    j["pcmBitsByFormat"] = pcmBits;
    j["convertWhenUnsupported"] = s.convertWhenUnsupported;
    j["resampleQuality"] = resampleQualityId(s.resampleQuality);
    j["convertDsdWhenUnsupported"] = s.convertDsdWhenUnsupported;
    j["language"] = s.language;
    j["closeAction"] = s.closeAction == CloseAction::Exit ? "exit" : "tray";
    j["theme"] = s.theme == Theme::Light ? "light" : s.theme == Theme::Dark ? "dark" : "system";
    j["trayNoticeShown"] = s.trayNoticeShown;
    j["firstRunDone"] = s.firstRunDone;
    j["autoplay"] = s.autoplay;
    j["checkUpdates"] = s.checkUpdates;
    j["artistPictures"] = s.artistPictures == OnlineLookup::Allowed   ? "allowed"
                          : s.artistPictures == OnlineLookup::Refused ? "refused"
                                                                      : "ask";
    j["folderTree"] = s.folderTree;
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
    j["dspOrder"] = json::array();
    for (const auto node : s.dspOrder) j["dspOrder"].push_back(kDspNodeIds[int(node) - 1]);
    j["lastFile"] = toUtf8(s.lastFile);
    j["wasapi"] = {{"device", s.wasapi.deviceId},
                   {"shareMode", s.wasapi.shared ? "shared" : "exclusive"},
                   {"sharedResampleQuality", resampleQualityId(s.wasapi.sharedResampleQuality)},
                   {"eventDriven", s.wasapi.eventDriven},
                   {"period", periodId(s.wasapi.period)},
                   {"customPeriodMs", s.wasapi.customPeriodMs},
                   {"sampleType", s.wasapi.sampleType ? sampleTypeId(*s.wasapi.sampleType) : ""},
                   {"dsdOutput", dsdOutputId(s.wasapi.dsdOutput)},
                   {"dsdPcmRate", s.wasapi.dsdPcmRate},
                   {"fixedRate", s.wasapi.fixedRate},
                   {"pollBufferMs", s.wasapi.pollBufferMs},
                   {"mmcssTask", mmcssTaskId(s.wasapi.mmcssTask)},
                   {"mmcssPriority", mmcssPriorityId(s.wasapi.mmcssPriority)}};
    j["wasapi"].update(timingJson(s.wasapi.timing));
    j["asio"] = {{"driver", s.asio.driverClsid},
                 {"bufferSize", s.asio.bufferSize},
                 {"outLeft", s.asio.outLeft},
                 {"outRight", s.asio.outRight},
                 {"dsdOutput", dsdOutputId(s.asio.dsdOutput)},
                 {"dsdPcmRate", s.asio.dsdPcmRate},
                 {"fixedRate", s.asio.fixedRate}};
    j["asio"].update(timingJson(s.asio.timing));
    j["ks"] = {{"device", s.ks.deviceId},
               {"mode", ksModeId(s.ks.mode)},
               {"bufferMs", s.ks.bufferMs},
               {"packetCount", s.ks.packetCount},
               {"packetMs", s.ks.packetMs},
               {"sampleType", s.ks.sampleType ? sampleTypeId(*s.ks.sampleType) : ""},
               {"dsdOutput", dsdOutputId(s.ks.dsdOutput)},
               {"dsdPcmRate", s.ks.dsdPcmRate},
               {"fixedRate", s.ks.fixedRate},
               {"mmcssTask", mmcssTaskId(s.ks.mmcssTask)},
               {"mmcssPriority", mmcssPriorityId(s.ks.mmcssPriority)}};
    j["ks"].update(timingJson(s.ks.timing));
    return j.dump(2);
}

Settings fromJson(const std::string& text) {
    json j = json::parse(text);
    if (!j.is_object()) throw std::invalid_argument("settings must be a JSON object");
    Settings s;
    if (j.contains("schema") && !j["schema"].is_number_integer()) throw std::invalid_argument("invalid settings schema");
    const int schema = j.contains("schema") ? j["schema"].get<int>() : 0;
    if (schema < 0 || schema > Settings::kSchemaVersion) throw std::invalid_argument("unsupported settings schema");
    auto str = [](const json& o, const char* k, const std::string& def) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : def;
    };
    auto num = [](const json& o, const char* k, double def, double minimum, double maximum) {
        if (!o.contains(k) || !o[k].is_number()) return def;
        const double value = o[k].get<double>();
        return std::isfinite(value) && value >= minimum && value <= maximum ? value : def;
    };
    auto integer = [&](const json& o, const char* k, int def, int minimum, int maximum) {
        const double value = num(o, k, def, minimum, maximum);
        return std::trunc(value) == value ? int(value) : def;
    };
    auto boolean = [](const json& o, const char* k, bool def) {
        return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def;
    };
    auto readTiming = [&](const json& o, PlaybackTiming& t) {
        t.predecodeSeconds = uint32_t(integer(o, "predecodeSeconds", 10, 1, 60));
        t.formatChangeSilenceMs = uint32_t(integer(o, "formatChangeSilenceMs", 0, 0, 2000));
        t.releaseAfterPauseSeconds = uint32_t(integer(o, "releaseAfterPauseSeconds", 1, 0, 3600));
    };
    backendFromId(str(j, "backend", "wasapi"), s.backend);
    // A choice survives only when that decoder is integrated for that format.
    auto choose = [&](std::string_view formatId, std::string_view decoderId) {
        audio::AudioFormat format{};
        audio::DecoderKind decoder{};
        if (audio::formatFromId(formatId, format) && audio::decoderFromId(decoderId, decoder) && audio::supports(format, decoder))
            s.decoderByFormat[format] = decoder;
    };
    if (schema < 3) {
        if (j.contains("decoder") && j["decoder"].is_string()) choose("flac", j["decoder"].get<std::string>());
    } else if (j.contains("decoderByFormat") && j["decoderByFormat"].is_object()) {
        for (const auto& [formatId, decoderId] : j["decoderByFormat"].items())
            if (decoderId.is_string()) choose(formatId, decoderId.get<std::string>());
    }
    if (j.contains("pcmBitsByFormat") && j["pcmBitsByFormat"].is_object())
        for (const auto& [formatId, bits] : j["pcmBitsByFormat"].items()) {
            audio::AudioFormat format{};
            if (audio::formatFromId(formatId, format) && audio::isLossy(format) && bits.is_number_integer() &&
                (bits.get<int64_t>() == 16 || bits.get<int64_t>() == 24))
                s.pcmBitsByFormat[format] = uint32_t(bits.get<int64_t>());
        }
    s.convertWhenUnsupported = boolean(j, "convertWhenUnsupported", false);
    s.resampleQuality = resampleQualityFromId(str(j, "resampleQuality", "high"));
    s.convertDsdWhenUnsupported = boolean(j, "convertDsdWhenUnsupported", false);
    s.language = str(j, "language", "");
    s.closeAction = str(j, "closeAction", "tray") == "exit" ? CloseAction::Exit : CloseAction::Tray;
    const auto theme = str(j, "theme", "system");
    s.theme = theme == "light" ? Theme::Light : theme == "dark" ? Theme::Dark : Theme::System;
    s.trayNoticeShown = boolean(j, "trayNoticeShown", false);
    s.firstRunDone = boolean(j, "firstRunDone", false);
    s.autoplay = boolean(j, "autoplay", true);
    s.checkUpdates = boolean(j, "checkUpdates", true);
    const auto pictures = str(j, "artistPictures", "ask");
    s.artistPictures = pictures == "allowed"   ? OnlineLookup::Allowed
                       : pictures == "refused" ? OnlineLookup::Refused
                                               : OnlineLookup::Ask;
    s.folderTree = boolean(j, "folderTree", false);
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
    if (j.contains("dspOrder") && j["dspOrder"].is_array() && j["dspOrder"].size() == 3) {
        audio::DspOrder order{};
        for (size_t i = 0; i < 3; ++i) {
            const auto& item = j["dspOrder"][i];
            const auto id = std::ranges::find(kDspNodeIds, item.is_string() ? item.get<std::string>() : std::string());
            order[i] = id == std::end(kDspNodeIds) ? audio::DspNode{} : audio::DspNode(id - std::begin(kDspNodeIds) + 1);
        }
        auto sorted = order;
        std::ranges::sort(sorted);
        if (sorted == audio::kDefaultDspOrder) s.dspOrder = order;
    }
    s.lastFile = toWide(str(j, "lastFile", ""));
    if (j.contains("wasapi") && j["wasapi"].is_object()) {
        const json& w = j["wasapi"];
        s.wasapi.deviceId = str(w, "device", "");
        s.wasapi.shared = str(w, "shareMode", "exclusive") == "shared";
        s.wasapi.sharedResampleQuality = resampleQualityFromId(str(w, "sharedResampleQuality", "high"));
        s.wasapi.eventDriven = boolean(w, "eventDriven", true);
        s.wasapi.period = periodFromId(str(w, "period", "default"));
        s.wasapi.customPeriodMs = num(w, "customPeriodMs", 10.0, 1.0, 500.0);
        s.wasapi.sampleType = sampleTypeFromId(str(w, "sampleType", ""));
        s.wasapi.dsdOutput = dsdOutputFromId(str(w, "dsdOutput", ""), DsdOutput::Dop);
        if (s.wasapi.dsdOutput == DsdOutput::Native) s.wasapi.dsdOutput = DsdOutput::Dop; // WASAPI has no native DSD
        s.wasapi.dsdPcmRate = dsdPcmRateFrom(num(w, "dsdPcmRate", 176400, 0, 1e7));
        s.wasapi.fixedRate = fixedRateFrom(num(w, "fixedRate", 0, 0, 1e7));
        s.wasapi.pollBufferMs = num(w, "pollBufferMs", 200.0, 20.0, 2000.0);
        s.wasapi.mmcssTask = mmcssTaskFromId(str(w, "mmcssTask", ""));
        s.wasapi.mmcssPriority = mmcssPriorityFromId(str(w, "mmcssPriority", ""));
        readTiming(w, s.wasapi.timing);
    }
    if (j.contains("asio") && j["asio"].is_object()) {
        const json& a = j["asio"];
        s.asio.driverClsid = str(a, "driver", "");
        s.asio.bufferSize = integer(a, "bufferSize", 0, 0, 65536);
        s.asio.outLeft = integer(a, "outLeft", 0, 0, 63);
        s.asio.outRight = integer(a, "outRight", 1, 0, 63);
        s.asio.dsdOutput = dsdOutputFromId(str(a, "dsdOutput", ""), DsdOutput::Native);
        s.asio.dsdPcmRate = dsdPcmRateFrom(num(a, "dsdPcmRate", 176400, 0, 1e7));
        s.asio.fixedRate = fixedRateFrom(num(a, "fixedRate", 0, 0, 1e7));
        readTiming(a, s.asio.timing);
    }
    if (j.contains("ks") && j["ks"].is_object()) {
        const json& k = j["ks"];
        s.ks.deviceId = str(k, "device", "");
        s.ks.mode = ksModeFromId(str(k, "mode", "auto"));
        s.ks.bufferMs = num(k, "bufferMs", 20.0, 2.0, 1000.0);
        s.ks.packetCount = integer(k, "packetCount", 4, 2, 16);
        s.ks.packetMs = num(k, "packetMs", 10.0, 1.0, 200.0);
        s.ks.sampleType = sampleTypeFromId(str(k, "sampleType", ""));
        s.ks.dsdOutput = dsdOutputFromId(str(k, "dsdOutput", ""), DsdOutput::Dop);
        if (s.ks.dsdOutput == DsdOutput::Native) s.ks.dsdOutput = DsdOutput::Dop; // Windows defines no KS DSD format
        s.ks.dsdPcmRate = dsdPcmRateFrom(num(k, "dsdPcmRate", 176400, 0, 1e7));
        s.ks.fixedRate = fixedRateFrom(num(k, "fixedRate", 0, 0, 1e7));
        s.ks.mmcssTask = mmcssTaskFromId(str(k, "mmcssTask", ""));
        s.ks.mmcssPriority = mmcssPriorityFromId(str(k, "mmcssPriority", ""));
        readTiming(k, s.ks.timing);
    }
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
    return reset;
}

std::filesystem::path settingsPath() {
    return dataDirectory() / L"settings.json";
}

namespace {

// Nothing for a missing file; throws NewerSettingsError, or another exception for a corrupt file.
std::optional<Settings> readSettings(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
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
