#include "Common/settings/Settings.h"

#include <windows.h>
#include <shlobj.h>

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
    j["language"] = s.language;
    j["lastFile"] = toUtf8(s.lastFile);
    j["wasapi"] = {{"device", s.wasapi.deviceId},
                   {"eventDriven", s.wasapi.eventDriven},
                   {"period", periodId(s.wasapi.period)},
                   {"customPeriodMs", s.wasapi.customPeriodMs},
                   {"sampleType", s.wasapi.sampleType ? sampleTypeId(*s.wasapi.sampleType) : ""}};
    j["asio"] = {{"driver", s.asio.driverClsid},
                 {"bufferSize", s.asio.bufferSize},
                 {"outLeft", s.asio.outLeft},
                 {"outRight", s.asio.outRight}};
    j["ks"] = {{"device", s.ks.deviceId},   {"mode", ksModeId(s.ks.mode)},
               {"bufferMs", s.ks.bufferMs}, {"packetCount", s.ks.packetCount},
               {"packetMs", s.ks.packetMs}, {"sampleType", s.ks.sampleType ? sampleTypeId(*s.ks.sampleType) : ""}};
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
    s.language = str(j, "language", "");
    s.lastFile = toWide(str(j, "lastFile", ""));
    if (j.contains("wasapi") && j["wasapi"].is_object()) {
        const json& w = j["wasapi"];
        s.wasapi.deviceId = str(w, "device", "");
        s.wasapi.eventDriven = boolean(w, "eventDriven", true);
        s.wasapi.period = periodFromId(str(w, "period", "default"));
        s.wasapi.customPeriodMs = num(w, "customPeriodMs", 10.0, 1.0, 500.0);
        s.wasapi.sampleType = sampleTypeFromId(str(w, "sampleType", ""));
    }
    if (j.contains("asio") && j["asio"].is_object()) {
        const json& a = j["asio"];
        s.asio.driverClsid = str(a, "driver", "");
        s.asio.bufferSize = integer(a, "bufferSize", 0, 0, 65536);
        s.asio.outLeft = integer(a, "outLeft", 0, 0, 63);
        s.asio.outRight = integer(a, "outRight", 1, 0, 63);
    }
    if (j.contains("ks") && j["ks"].is_object()) {
        const json& k = j["ks"];
        s.ks.deviceId = str(k, "device", "");
        s.ks.mode = ksModeFromId(str(k, "mode", "auto"));
        s.ks.bufferMs = num(k, "bufferMs", 20.0, 2.0, 1000.0);
        s.ks.packetCount = integer(k, "packetCount", 4, 2, 16);
        s.ks.packetMs = num(k, "packetMs", 10.0, 1.0, 200.0);
        s.ks.sampleType = sampleTypeFromId(str(k, "sampleType", ""));
    }
    return s;
}

std::filesystem::path dataDirectory() {
    PWSTR base = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base)))
        dir = std::filesystem::path(base) / L"CamelPlayer";
    CoTaskMemFree(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path settingsPath() {
    return dataDirectory() / L"settings.json";
}

Settings loadSettings(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    try {
        const auto contents = ss.str();
        const auto parsed = json::parse(contents);
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
    } catch (const std::exception& e) {
        log::warn("settings: corrupt file, using defaults (%s)", e.what());
        std::error_code ec;
        std::filesystem::rename(path, std::filesystem::path(path).concat(L".bad"), ec);
        return {};
    }
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
