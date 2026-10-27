// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/DiagnosticBundle.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Common/DecoderKind.h"
#include "Common/util/Text.h"
#include "Server/Support/ZipWriter.h"

namespace camel::server {

namespace {

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::wstring registryText(const wchar_t* name) {
    std::array<wchar_t, 256> value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ, nullptr,
                     value.data(), &size) != ERROR_SUCCESS)
        return {};
    return value.data();
}

DWORD registryNumber(const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_DWORD, nullptr, &value,
                 &size);
    return value;
}

std::string systemText(const DiagnosticInput& input) {
    const auto build = registryText(L"CurrentBuildNumber");
    std::ostringstream out;
    out << "CamelPlayer " << input.appVersion << "\n";
    // Windows 11 still reports "Windows 10" as ProductName; build 22000 and later is Windows 11.
    out << "Windows " << (!build.empty() && std::stoul(build) >= 22000 ? "11" : "10") << " "
        << toUtf8(registryText(L"DisplayVersion")) << ", build " << toUtf8(build) << "." << registryNumber(L"UBR") << " ("
        << toUtf8(registryText(L"EditionID")) << ")\n";
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    out << "Processors: " << info.dwNumberOfProcessors << "\n";
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::array<char, 32> time{};
    snprintf(time.data(), time.size(), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    out << "Created: " << time.data() << "\n";
    if (input.includePaths) out << "Data folder: " << toUtf8(input.dataDirectory.wstring()) << "\n";
    return out.str();
}

std::string settingsText(const DiagnosticInput& input) {
    auto text = readFile(input.dataDirectory / L"settings.json");
    if (input.includePaths || text.empty()) return text;
    try {
        auto json = nlohmann::json::parse(text);
        json.erase("lastFile");
        return json.dump(2);
    } catch (const std::exception&) {
        return "settings.json is not valid JSON\n";
    }
}

std::string rootsText(const DiagnosticInput& input) {
    std::ostringstream out;
    for (const auto& root : input.roots) {
        out << "Root " << root.id << ": " << (root.online ? "online" : "offline") << ", "
            << (root.path.starts_with("\\\\") ? "network" : "local") << ", last scan " << root.lastScan;
        if (input.includePaths) out << ", " << root.path;
        out << "\n";
    }
    if (input.roots.empty()) out << "No library folders\n";
    return out.str();
}

std::string playbackText(const DiagnosticInput& input) {
    const auto& p = input.snapshot.playback;
    std::ostringstream out;
    out << "State: "
        << (p.state == PlaybackState::Playing  ? "playing"
            : p.state == PlaybackState::Paused ? "paused"
                                               : "stopped")
        << "\n";
    out << "Backend: " << backendName(p.backend);
    if (p.activeBackend) out << " (active " << backendName(*p.activeBackend) << ")";
    out << "\nDecoder: " << audio::decoderName(p.decoder);
    if (p.activeDecoder) out << " (active " << audio::decoderName(*p.activeDecoder) << ")";
    out << "\n";
    if (input.includePaths && !p.pathUtf8.empty()) out << "File: " << p.pathUtf8 << "\n";
    if (p.source)
        out << "Source: " << p.source->sampleRate << " Hz, " << p.source->channels << " ch, " << p.source->bitsPerSample
            << " bit\n";
    if (p.output) out << "Output: " << audio::describe(*p.output) << "\n";
    if (!p.outputMode.empty()) out << "Mode: " << p.outputMode << "\n";
    if (!p.outputDetail.empty()) out << "Detail: " << p.outputDetail << "\n";
    for (const auto& adjustment : p.outputAdjustments) out << "Adjusted: " << adjustment << "\n";
    out << "Buffer frames: " << p.bufferFrames << "\nUnderruns: " << p.underruns << "\n";
    if (p.error) out << "Error: " << audio::errorCodeName(*p.error) << " " << p.errorDetail << "\n";
    return input.includePaths ? out.str() : redactPaths(out.str());
}

std::string devicesText(const DiagnosticInput& input) {
    std::ostringstream out;
    const auto list = [&](const char* title, const std::vector<DeviceInfo>& devices) {
        out << title << ":\n";
        if (devices.empty()) out << "  (none)\n";
        for (const auto& device : devices) out << "  " << device.name << "\n    id: " << device.id << "\n";
    };
    list("WASAPI endpoints", input.snapshot.devices.wasapi);
    list("ASIO drivers", input.snapshot.devices.asio);
    list("Kernel Streaming pins", input.snapshot.devices.ks);
    out << "\n" << input.capabilities;
    return out.str();
}

bool isPathStart(std::string_view text, size_t at) {
    if (text.substr(at).starts_with("\\\\")) return true;
    if (at + 2 >= text.size() || !std::isalpha(uint8_t(text[at])) || text[at + 1] != ':') return false;
    if (text[at + 2] != '\\' && text[at + 2] != '/') return false;
    return at == 0 || !std::isalnum(uint8_t(text[at - 1]));
}

} // namespace

std::string redactPaths(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t at = 0;
    while (at < text.size()) {
        size_t start = at;
        while (start < text.size() && !isPathStart(text, start)) ++start;
        out.append(text.substr(at, start - at));
        if (start == text.size()) break;
        size_t end = std::min(text.find_first_of("\r\n", start), text.size());
        for (const std::string_view after : {" via ", " is reported ", ": "}) end = std::min(end, text.find(after, start));
        out += "<path>";
        at = end;
    }
    return out;
}

std::string probeCapabilities(audio::IOutput& output, const Settings& settings) {
    using audio::SampleType;
    constexpr std::array types{SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32,
                               SampleType::Float32};
    std::ostringstream out;
    out << "Formats " << backendName(settings.backend) << " accepts on the selected device (stereo):\n";
    out << "rate";
    for (const auto type : types) out << "\t" << audio::sampleTypeName(type);
    out << "\n";
    for (const uint32_t rate : kFixedRates) {
        out << rate;
        for (const auto type : types) out << "\t" << (output.supports({rate, 2, type}) ? "yes" : "-");
        out << "\n";
    }
    if (settings.backend == Backend::Asio) {
        out << "Native DSD:";
        for (const uint32_t multiple : {64, 128, 256, 512}) {
            const uint32_t rate = 44100 * multiple;
            out << " DSD" << multiple << " " << (output.supports({rate / 8, 2, SampleType::Dsd8}) ? "yes" : "-");
        }
        out << "\n";
    }
    return out.str();
}

void writeDiagnosticBundle(const std::filesystem::path& zip, const DiagnosticInput& input) {
    try {
        ZipWriter writer(zip);
        writer.add("system.txt", systemText(input));
        writer.add("settings.json", settingsText(input));
        writer.add("library-folders.txt", rootsText(input));
        writer.add("playback.txt", playbackText(input));
        writer.add("devices.txt", devicesText(input));
        std::error_code ec;
        std::vector<std::filesystem::path> logs, dumps;
        for (const auto& entry : std::filesystem::directory_iterator(input.dataDirectory / L"logs", ec))
            if (entry.path().extension() == L".log") logs.push_back(entry.path());
        for (const auto& entry : std::filesystem::directory_iterator(input.dataDirectory / L"crashdumps", ec))
            if (entry.path().filename().wstring().starts_with(L"crash-") && entry.path().extension() == L".dmp")
                dumps.push_back(entry.path());
        std::sort(logs.begin(), logs.end());
        for (const auto& log : logs) {
            const auto text = readFile(log);
            writer.add("logs/" + toUtf8(log.filename().wstring()), input.includePaths ? text : redactPaths(text));
        }
        // crash-YYYYMMDD-hhmmss.dmp sorts by time.
        if (!dumps.empty()) {
            const auto newest = *std::max_element(dumps.begin(), dumps.end());
            writer.add("crashdumps/" + toUtf8(newest.filename().wstring()), readFile(newest));
        }
        writer.finish();
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(zip, ec);
        throw;
    }
}

} // namespace camel::server
