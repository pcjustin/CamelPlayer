// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Api/ApiJson.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace camel::server {
namespace {

using nlohmann::json;
using namespace camel::audio;

[[noreturn]] void invalid(const std::string& what) {
    throw std::invalid_argument("API JSON: " + what);
}

const json& field(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) invalid(std::string("missing ") + key);
    return j[key];
}

template <class T> T value(const json& j, const char* key) {
    const json& v = field(j, key);
    if constexpr (std::is_same_v<T, bool>) {
        if (!v.is_boolean()) invalid(std::string(key) + " is not a boolean");
    } else if constexpr (std::is_integral_v<T>) {
        if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<int64_t>() >= 0))
            invalid(std::string(key) + " is not an unsigned integer");
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!v.is_string()) invalid(std::string(key) + " is not a string");
    }
    return v.get<T>();
}

const json& array(const json& j, const char* key) {
    const json& v = field(j, key);
    if (!v.is_array()) invalid(std::string(key) + " is not an array");
    return v;
}

// `read` turns a present, non-null value into T.
template <class T, class F> std::optional<T> optional(const json& j, const char* key, F read) {
    const json& v = field(j, key);
    if (v.is_null()) return std::nullopt;
    return read(v);
}

template <class E, size_t N> using Names = std::pair<E, const char*>[N];

template <class E, size_t N> const char* nameOf(E e, const Names<E, N>& names) {
    for (const auto& [v, n] : names)
        if (v == e) return n;
    return "?";
}

template <class E, size_t N> E enumFrom(const json& v, const Names<E, N>& names, const char* what) {
    if (!v.is_string()) invalid(std::string(what) + " is not a string");
    const auto s = v.get<std::string>();
    for (const auto& [e, n] : names)
        if (s == n) return e;
    invalid("unknown " + std::string(what) + " " + s);
}

constexpr Names<CommandKind, 25> kCommandKinds = {{CommandKind::OpenAndPlay, "openAndPlay"},
                                                  {CommandKind::Load, "load"},
                                                  {CommandKind::Play, "play"},
                                                  {CommandKind::Pause, "pause"},
                                                  {CommandKind::TogglePause, "togglePause"},
                                                  {CommandKind::Stop, "stop"},
                                                  {CommandKind::Seek, "seek"},
                                                  {CommandKind::ApplySettings, "applySettings"},
                                                  {CommandKind::RefreshDevices, "refreshDevices"},
                                                  {CommandKind::ShowControlPanel, "showControlPanel"},
                                                  {CommandKind::QueueReplace, "queueReplace"},
                                                  {CommandKind::QueueReorder, "queueReorder"},
                                                  {CommandKind::QueueSelect, "queueSelect"},
                                                  {CommandKind::Next, "next"},
                                                  {CommandKind::Previous, "previous"},
                                                  {CommandKind::SetShuffle, "setShuffle"},
                                                  {CommandKind::SetRepeat, "setRepeat"},
                                                  {CommandKind::RemoveLibraryRoot, "removeLibraryRoot"},
                                                  {CommandKind::ResetPlayStatistics, "resetPlayStatistics"},
                                                  {CommandKind::ResetAllPlayStatistics, "resetAllPlayStatistics"},
                                                  {CommandKind::AddLibraryRoot, "addLibraryRoot"},
                                                  {CommandKind::QueueAdd, "queueAdd"},
                                                  {CommandKind::QueuePlayNext, "queuePlayNext"},
                                                  {CommandKind::QueueRemove, "queueRemove"},
                                                  {CommandKind::QueueClear, "queueClear"}};
constexpr Names<RepeatMode, 3> kRepeatModes = {{RepeatMode::Off, "off"}, {RepeatMode::One, "one"}, {RepeatMode::All, "all"}};
constexpr Names<PlaybackState, 3> kPlaybackStates = {
    {PlaybackState::Stopped, "stopped"}, {PlaybackState::Playing, "playing"}, {PlaybackState::Paused, "paused"}};
constexpr Names<EventKind, 2> kEventKinds = {{EventKind::StateChanged, "stateChanged"},
                                             {EventKind::CommandCompleted, "commandCompleted"}};
constexpr Names<ErrorCode, 10> kErrorCodes = {{ErrorCode::FileOpen, "fileOpen"},
                                              {ErrorCode::UnsupportedStream, "unsupportedStream"},
                                              {ErrorCode::DecodeFailed, "decodeFailed"},
                                              {ErrorCode::SeekFailed, "seekFailed"},
                                              {ErrorCode::DeviceNotFound, "deviceNotFound"},
                                              {ErrorCode::DeviceInUse, "deviceInUse"},
                                              {ErrorCode::DeviceLost, "deviceLost"},
                                              {ErrorCode::FormatNotSupported, "formatNotSupported"},
                                              {ErrorCode::DriverError, "driverError"},
                                              {ErrorCode::Other, "other"}};
constexpr Names<SampleType, 6> kSampleTypes = {{SampleType::Int16, "16"},         {SampleType::Int24, "24"},
                                               {SampleType::Int24In32, "24in32"}, {SampleType::Int32, "32"},
                                               {SampleType::Float32, "f32"},      {SampleType::Dsd8, "dsd8"}};
constexpr Names<PipelineState, 4> kPipelineStates = {{PipelineState::BitPerfect, "bitPerfect"},
                                                     {PipelineState::LosslessConversion, "losslessConversion"},
                                                     {PipelineState::Processed, "processed"},
                                                     {PipelineState::LossySource, "lossySource"}};
constexpr Names<Md5State, 4> kMd5States = {{Md5State::Unavailable, "unavailable"},
                                           {Md5State::Incomplete, "incomplete"},
                                           {Md5State::Verified, "verified"},
                                           {Md5State::Failed, "failed"}};
constexpr Names<DsdPath, 4> kDsdPaths = {
    {DsdPath::None, "none"}, {DsdPath::Native, "native"}, {DsdPath::Dop, "dop"}, {DsdPath::Pcm, "pcm"}};

Backend backendFrom(const json& v) {
    Backend b{};
    if (!v.is_string() || !backendFromId(v.get<std::string>(), b)) invalid("unknown backend");
    return b;
}

DecoderKind decoderFrom(const json& v) {
    DecoderKind d{};
    if (!v.is_string() || !decoderFromId(v.get<std::string>(), d)) invalid("unknown decoder");
    return d;
}

AudioFormat formatFrom(const json& v) {
    AudioFormat f{};
    if (!v.is_string() || !formatFromId(v.get<std::string>(), f)) invalid("unknown format");
    return f;
}

json toJson(const TrackSource& s) {
    return {{"path", s.pathUtf8()},
            {"first", s.range().first},
            {"end", s.range().endExclusive ? json(*s.range().endExclusive) : json(nullptr)},
            {"decoder", s.decoder() ? json(decoderId(*s.decoder())) : json(nullptr)}};
}

TrackSource trackSourceFrom(const json& j) {
    FrameRange range{value<uint64_t>(j, "first"), optional<uint64_t>(j, "end", [](const json& v) {
                         if (!v.is_number_unsigned()) invalid("end is not an unsigned integer");
                         return v.get<uint64_t>();
                     })};
    return TrackSource(value<std::string>(j, "path"), range, optional<DecoderKind>(j, "decoder", decoderFrom));
}

json toJson(const SettingsDto& s) {
    return json::parse(camel::toJson(nativeSettings(s)));
}

SettingsDto settingsFrom(const json& j) {
    if (!j.is_object()) invalid("settings is not an object");
    return settingsDto(camel::fromJson(j.dump()));
}

json toJson(const Failure& f) {
    return {{"code", nameOf(f.code, kErrorCodes)}, {"detail", f.detail}};
}

Failure failureFrom(const json& j) {
    return {enumFrom(field(j, "code"), kErrorCodes, "error code"), value<std::string>(j, "detail")};
}

json toJson(const SourceInfo& s) {
    json md5 = nullptr;
    if (s.md5) {
        std::string hex;
        for (uint8_t b : *s.md5) {
            hex += "0123456789abcdef"[b >> 4];
            hex += "0123456789abcdef"[b & 15];
        }
        md5 = hex;
    }
    return {{"sampleRate", s.sampleRate},
            {"channels", s.channels},
            {"bitsPerSample", s.bitsPerSample},
            {"totalFrames", s.totalFrames},
            {"md5", md5}};
}

SourceInfo sourceFrom(const json& j) {
    SourceInfo s;
    s.sampleRate = value<uint32_t>(j, "sampleRate");
    s.channels = value<uint32_t>(j, "channels");
    s.bitsPerSample = value<uint32_t>(j, "bitsPerSample");
    s.totalFrames = value<uint64_t>(j, "totalFrames");
    s.md5 = optional<std::array<uint8_t, 16>>(j, "md5", [](const json& v) {
        if (!v.is_string() || v.get<std::string>().size() != 32) invalid("md5 is not 32 hex digits");
        const auto hex = v.get<std::string>();
        std::array<uint8_t, 16> out{};
        for (size_t i = 0; i < 16; i++) {
            const auto byte = hex.substr(i * 2, 2);
            if (byte.find_first_not_of("0123456789abcdef") != std::string::npos) invalid("md5 is not lowercase hex");
            out[i] = uint8_t(std::stoul(byte, nullptr, 16));
        }
        return out;
    });
    return s;
}

json toJson(const DeviceFormat& f) {
    return {{"sampleRate", f.sampleRate}, {"channels", f.channels}, {"type", nameOf(f.type, kSampleTypes)}};
}

DeviceFormat deviceFormatFrom(const json& j) {
    return {value<uint32_t>(j, "sampleRate"), value<uint32_t>(j, "channels"),
            enumFrom(field(j, "type"), kSampleTypes, "sample type")};
}

json toJson(const OutputPlan& p) {
    return {{"format", toJson(p.format)},      {"duplicate", p.duplicate}, {"downmix", p.downmix},
            {"resample", p.resample},          {"widen", p.widen},         {"reduce", p.reduce},
            {"dsd", nameOf(p.dsd, kDsdPaths)}, {"shared", p.shared},       {"state", nameOf(p.state, kPipelineStates)}};
}

OutputPlan planFrom(const json& j) {
    OutputPlan p;
    p.format = deviceFormatFrom(field(j, "format"));
    p.duplicate = value<bool>(j, "duplicate");
    p.downmix = value<bool>(j, "downmix");
    p.resample = value<bool>(j, "resample");
    p.widen = value<bool>(j, "widen");
    p.reduce = value<bool>(j, "reduce");
    p.dsd = enumFrom(field(j, "dsd"), kDsdPaths, "DSD path");
    p.shared = value<bool>(j, "shared");
    p.state = enumFrom(field(j, "state"), kPipelineStates, "pipeline state");
    return p;
}

template <class T> json optionalJson(const std::optional<T>& v) {
    return v ? toJson(*v) : json(nullptr);
}

json toJson(const PlaybackSnapshot& p) {
    return {{"state", nameOf(p.state, kPlaybackStates)},
            {"path", p.pathUtf8},
            {"position", p.position},
            {"total", p.total},
            {"source", optionalJson(p.source)},
            {"format", p.format ? json(formatId(*p.format)) : json(nullptr)},
            {"output", optionalJson(p.output)},
            {"plan", optionalJson(p.plan)},
            {"md5", p.md5 ? json(nameOf(*p.md5, kMd5States)) : json(nullptr)},
            {"backend", backendId(p.backend)},
            {"decoder", decoderId(p.decoder)},
            {"activeBackend", p.activeBackend ? json(backendId(*p.activeBackend)) : json(nullptr)},
            {"activeDecoder", p.activeDecoder ? json(decoderId(*p.activeDecoder)) : json(nullptr)},
            {"outputMode", p.outputMode},
            {"outputDetail", p.outputDetail},
            {"outputAdjustments", p.outputAdjustments},
            {"bufferFrames", p.bufferFrames},
            {"underruns", p.underruns},
            {"error", p.error ? json(nameOf(*p.error, kErrorCodes)) : json(nullptr)},
            {"errorDetail", p.errorDetail},
            {"revision", p.revision}};
}

PlaybackSnapshot playbackFrom(const json& j) {
    PlaybackSnapshot p;
    p.state = enumFrom(field(j, "state"), kPlaybackStates, "playback state");
    p.pathUtf8 = value<std::string>(j, "path");
    p.position = value<uint64_t>(j, "position");
    p.total = value<uint64_t>(j, "total");
    p.source = optional<SourceInfo>(j, "source", sourceFrom);
    p.format = optional<AudioFormat>(j, "format", formatFrom);
    p.output = optional<DeviceFormat>(j, "output", deviceFormatFrom);
    p.plan = optional<OutputPlan>(j, "plan", planFrom);
    p.md5 = optional<Md5State>(j, "md5", [](const json& v) { return enumFrom(v, kMd5States, "MD5 state"); });
    p.backend = backendFrom(field(j, "backend"));
    p.decoder = decoderFrom(field(j, "decoder"));
    p.activeBackend = optional<Backend>(j, "activeBackend", backendFrom);
    p.activeDecoder = optional<DecoderKind>(j, "activeDecoder", decoderFrom);
    p.outputMode = value<std::string>(j, "outputMode");
    p.outputDetail = value<std::string>(j, "outputDetail");
    for (const auto& a : array(j, "outputAdjustments")) {
        if (!a.is_string()) invalid("outputAdjustments holds a non-string");
        p.outputAdjustments.push_back(a.get<std::string>());
    }
    p.bufferFrames = value<uint32_t>(j, "bufferFrames");
    p.underruns = value<uint64_t>(j, "underruns");
    p.error = optional<ErrorCode>(j, "error", [](const json& v) { return enumFrom(v, kErrorCodes, "error code"); });
    p.errorDetail = value<std::string>(j, "errorDetail");
    p.revision = value<uint64_t>(j, "revision");
    return p;
}

json toJson(const std::vector<DeviceInfo>& devices) {
    json out = json::array();
    for (const auto& d : devices) out.push_back({{"id", d.id}, {"name", d.name}});
    return out;
}

std::vector<DeviceInfo> devicesFrom(const json& j, const char* key) {
    std::vector<DeviceInfo> out;
    for (const auto& d : array(j, key)) out.push_back({value<std::string>(d, "id"), value<std::string>(d, "name")});
    return out;
}

json toJson(const QueueSnapshot& q) {
    json entries = json::array();
    for (const auto& e : q.entries)
        entries.push_back({{"id", e.id}, {"source", toJson(e.source)}, {"title", e.title}, {"artist", e.artist},
                           {"album", e.album}, {"albumId", e.albumId}, {"automatic", e.automatic}});
    return {{"revision", q.revision},
            {"entries", entries},
            {"upcomingEntryId", q.upcomingEntryId ? json(*q.upcomingEntryId) : json(nullptr)},
            {"shuffle", q.shuffle},
            {"repeat", nameOf(q.repeat, kRepeatModes)}};
}

QueueSnapshot queueFrom(const json& j) {
    QueueSnapshot q;
    q.revision = value<uint64_t>(j, "revision");
    std::vector<QueueEntry> entries;
    for (const auto& e : array(j, "entries")) {
        QueueEntry entry{value<uint64_t>(e, "id"), trackSourceFrom(field(e, "source"))};
        if (e.contains("title")) entry.title = value<std::string>(e, "title");
        if (e.contains("artist")) entry.artist = value<std::string>(e, "artist");
        if (e.contains("album")) entry.album = value<std::string>(e, "album");
        if (e.contains("albumId")) entry.albumId = value<int64_t>(e, "albumId");
        if (e.contains("automatic")) entry.automatic = value<bool>(e, "automatic");
        entries.push_back(std::move(entry));
    }
    q.entries = std::move(entries);
    q.upcomingEntryId = optional<uint64_t>(j, "upcomingEntryId", [](const json& v) {
        if (!v.is_number_unsigned()) invalid("upcomingEntryId is not an unsigned integer");
        return v.get<uint64_t>();
    });
    q.shuffle = value<bool>(j, "shuffle");
    q.repeat = enumFrom(field(j, "repeat"), kRepeatModes, "repeat mode");
    return q;
}

std::vector<uint64_t> idsFrom(const json& j, const char* key) {
    std::vector<uint64_t> out;
    for (const auto& v : array(j, key)) {
        if (!v.is_number_unsigned()) invalid(std::string(key) + " holds a non-integer");
        out.push_back(v.get<uint64_t>());
    }
    return out;
}

std::vector<LibraryTrackKey> trackKeysFrom(const json& j, const char* key) {
    std::vector<LibraryTrackKey> out;
    for (const auto& v : array(j, key)) {
        if (!v.is_object()) invalid(std::string(key) + " holds a non-object");
        out.push_back({value<int64_t>(v, "file")});
    }
    return out;
}

} // namespace

json toJson(const CommandStamp& s) {
    return {{"id", s.id}, {"generation", s.generation}, {"queueRevision", s.queueRevision}};
}

CommandStamp commandStampFromJson(const json& j) {
    return {value<uint64_t>(j, "id"), value<uint64_t>(j, "generation"), value<uint64_t>(j, "queueRevision")};
}

json toJson(const Command& c) {
    json sources = json::array(), tracks = json::array();
    for (const auto& s : c.sources) sources.push_back(toJson(s));
    for (const auto& t : c.tracks) tracks.push_back({{"file", t.file}});
    return {{"apiVersion", c.apiVersion},
            {"kind", nameOf(c.kind, kCommandKinds)},
            {"source", optionalJson(c.source)},
            {"settings", optionalJson(c.settings)},
            {"frame", c.frame},
            {"sources", sources},
            {"index", c.index},
            {"entryIds", c.entryIds},
            {"entryId", c.entryId},
            {"rootId", c.rootId},
            {"shuffle", c.shuffle ? json(*c.shuffle) : json(nullptr)},
            {"repeat", c.repeat ? json(nameOf(*c.repeat, kRepeatModes)) : json(nullptr)},
            {"tracks", tracks},
            {"path", c.path}};
}

Command commandFromJson(const json& j) {
    Command c;
    c.apiVersion = value<uint32_t>(j, "apiVersion");
    c.kind = enumFrom(field(j, "kind"), kCommandKinds, "command kind");
    c.source = optional<TrackSource>(j, "source", trackSourceFrom);
    c.settings = optional<SettingsDto>(j, "settings", settingsFrom);
    c.frame = value<uint64_t>(j, "frame");
    for (const auto& s : array(j, "sources")) c.sources.push_back(trackSourceFrom(s));
    c.index = value<uint64_t>(j, "index");
    c.entryIds = idsFrom(j, "entryIds");
    c.entryId = value<uint64_t>(j, "entryId");
    if (j.contains("rootId") || c.kind == CommandKind::RemoveLibraryRoot) c.rootId = value<int64_t>(j, "rootId");
    c.shuffle = optional<bool>(j, "shuffle", [](const json& v) {
        if (!v.is_boolean()) invalid("shuffle is not a boolean");
        return v.get<bool>();
    });
    c.repeat = optional<RepeatMode>(j, "repeat", [](const json& v) { return enumFrom(v, kRepeatModes, "repeat mode"); });
    if (j.contains("tracks") || c.kind == CommandKind::ResetPlayStatistics) c.tracks = trackKeysFrom(j, "tracks");
    if (j.contains("path") || c.kind == CommandKind::AddLibraryRoot) c.path = value<std::string>(j, "path");
    return c;
}

json toJson(const Snapshot& s) {
    return {{"apiVersion", s.apiVersion},
            {"revision", s.revision},
            {"command", toJson(s.command)},
            {"queueEntryId", s.queueEntryId},
            {"track", optionalJson(s.track)},
            {"queue", toJson(s.queue)},
            {"playback", toJson(s.playback)},
            {"settings", toJson(s.settings)},
            {"devices",
             {{"revision", s.devices.revision},
              {"wasapi", toJson(s.devices.wasapi)},
              {"asio", toJson(s.devices.asio)},
              {"ks", toJson(s.devices.ks)}}},
            {"commandError", optionalJson(s.commandError)},
            {"controlPanelOpened", s.controlPanelOpened ? json(*s.controlPanelOpened) : json(nullptr)}};
}

Snapshot snapshotFromJson(const json& j) {
    Snapshot s;
    s.apiVersion = value<uint32_t>(j, "apiVersion");
    s.revision = value<uint64_t>(j, "revision");
    s.command = commandStampFromJson(field(j, "command"));
    s.queueEntryId = value<uint64_t>(j, "queueEntryId");
    s.track = optional<TrackSource>(j, "track", trackSourceFrom);
    s.queue = queueFrom(field(j, "queue"));
    s.playback = playbackFrom(field(j, "playback"));
    s.settings = settingsFrom(field(j, "settings"));
    const json& devices = field(j, "devices");
    s.devices.revision = value<uint64_t>(devices, "revision");
    s.devices.wasapi = devicesFrom(devices, "wasapi");
    s.devices.asio = devicesFrom(devices, "asio");
    s.devices.ks = devicesFrom(devices, "ks");
    s.commandError = optional<Failure>(j, "commandError", failureFrom);
    s.controlPanelOpened = optional<bool>(j, "controlPanelOpened", [](const json& v) {
        if (!v.is_boolean()) invalid("controlPanelOpened is not a boolean");
        return v.get<bool>();
    });
    return s;
}

json toJson(const Event& e) {
    return {{"kind", nameOf(e.kind, kEventKinds)}, {"snapshot", toJson(e.snapshot)}};
}

Event eventFromJson(const json& j) {
    return {enumFrom(field(j, "kind"), kEventKinds, "event kind"), snapshotFromJson(field(j, "snapshot"))};
}

} // namespace camel::server
