// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Api/ApiJson.h"

#include <algorithm>
#include <limits>
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

// An unsigned integer that T holds; one it does not is refused rather than cut to fit.
template <class T> T unsignedFrom(const json& v, const std::string& what) {
    if (!v.is_number_unsigned() && (!v.is_number_integer() || v.get<int64_t>() < 0))
        invalid(what + " is not an unsigned integer");
    if (v.get<uint64_t>() > uint64_t(std::numeric_limits<T>::max())) invalid(what + " is out of range");
    return T(v.get<uint64_t>());
}

template <class T> T value(const json& j, const char* key) {
    const json& v = field(j, key);
    if constexpr (std::is_same_v<T, bool>) {
        if (!v.is_boolean()) invalid(std::string(key) + " is not a boolean");
    } else if constexpr (std::is_integral_v<T>) {
        return unsignedFrom<T>(v, key);
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.is_number()) invalid(std::string(key) + " is not a number");
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

constexpr Names<CommandKind, 36> kCommandKinds = {{CommandKind::OpenAndPlay, "openAndPlay"},
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
                                                  {CommandKind::QueueClear, "queueClear"},
                                                  {CommandKind::SetFavorite, "setFavorite"},
                                                  {CommandKind::SetAlbumFavorite, "setAlbumFavorite"},
                                                  {CommandKind::SetArtistFavorite, "setArtistFavorite"},
                                                  {CommandKind::SetPlayerVolume, "setPlayerVolume"},
                                                  {CommandKind::SetPlayerMute, "setPlayerMute"},
                                                  {CommandKind::SetReceiverVolume, "setReceiverVolume"},
                                                  {CommandKind::ShowAirPlayCode, "showAirPlayCode"},
                                                  {CommandKind::PairAirPlay, "pairAirPlay"},
                                                  {CommandKind::DeleteOutput, "deleteOutput"},
                                                  {CommandKind::PauseAll, "pauseAll"},
                                                  {CommandKind::Transfer, "transfer"}};
constexpr Names<AirPlayChannels, 3> kAirPlayChannels = {
    {AirPlayChannels::Stereo, "stereo"}, {AirPlayChannels::Left, "left"}, {AirPlayChannels::Right, "right"}};
constexpr Names<LibraryGroupField, 2> kArtistFields = {{LibraryGroupField::Artist, "artist"},
                                                       {LibraryGroupField::Composer, "composer"}};
constexpr Names<RepeatMode, 3> kRepeatModes = {{RepeatMode::Off, "off"}, {RepeatMode::One, "one"}, {RepeatMode::All, "all"}};
constexpr Names<PlaybackState, 3> kPlaybackStates = {
    {PlaybackState::Stopped, "stopped"}, {PlaybackState::Playing, "playing"}, {PlaybackState::Paused, "paused"}};
constexpr Names<EventKind, 2> kEventKinds = {{EventKind::StateChanged, "stateChanged"},
                                             {EventKind::CommandCompleted, "commandCompleted"}};
constexpr Names<ErrorCode, 14> kErrorCodes = {{ErrorCode::FileOpen, "fileOpen"},
                                              {ErrorCode::UnsupportedStream, "unsupportedStream"},
                                              {ErrorCode::DecodeFailed, "decodeFailed"},
                                              {ErrorCode::SeekFailed, "seekFailed"},
                                              {ErrorCode::DeviceNotFound, "deviceNotFound"},
                                              {ErrorCode::DeviceInUse, "deviceInUse"},
                                              {ErrorCode::DeviceLost, "deviceLost"},
                                              {ErrorCode::FormatNotSupported, "formatNotSupported"},
                                              {ErrorCode::DriverError, "driverError"},
                                              {ErrorCode::AccessDenied, "accessDenied"},
                                              {ErrorCode::DeviceChanged, "deviceChanged"},
                                              {ErrorCode::TooSlow, "tooSlow"},
                                              {ErrorCode::CannotKeepUp, "cannotKeepUp"},
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
constexpr Names<FadeSkip, 4> kFadeSkips = {
    {FadeSkip::None, "none"}, {FadeSkip::RawDsd, "rawDsd"}, {FadeSkip::Format, "format"}, {FadeSkip::NoPreload, "noPreload"}};
constexpr Names<DspNode, 3> kDspNodes = {
    {DspNode::Eq, "eq"}, {DspNode::Crossfeed, "crossfeed"}, {DspNode::Convolution, "convolution"}};

Backend backendFrom(const json& v) {
    Backend b{};
    if (!v.is_string() || !backendFromId(v.get<std::string>(), b)) invalid("unknown backend");
    return b;
}

DecoderKind decoderFrom(const json& v) {
    DecoderKind d = DecoderKind::Sndfile;
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

json dspJson(const std::vector<DspNode>& nodes) {
    json out = json::array();
    for (const auto node : nodes) out.push_back(nameOf(node, kDspNodes));
    return out;
}

json toJson(const OutputPlan& p) {
    return {{"format", toJson(p.format)},
            {"duplicate", p.duplicate},
            {"downmix", p.downmix},
            {"resample", p.resample},
            {"widen", p.widen},
            {"reduce", p.reduce},
            {"dsd", nameOf(p.dsd, kDsdPaths)},
            {"remodulate", p.remodulate},
            {"modulatorOrder", p.modulatorOrder},
            {"shared", p.shared},
            {"dsp", dspJson(p.dsp)},
            {"gainDb", p.gainDb},
            {"volumeDb", p.volumeDb},
            {"muted", p.muted},
            {"crossfadeSeconds", p.crossfadeSeconds},
            {"fadeSkip", nameOf(p.fadeSkip, kFadeSkips)},
            {"state", nameOf(p.state, kPipelineStates)}};
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
    p.remodulate = value<bool>(j, "remodulate");
    p.modulatorOrder = value<uint32_t>(j, "modulatorOrder");
    p.shared = value<bool>(j, "shared");
    for (const auto& node : array(j, "dsp")) p.dsp.push_back(enumFrom(node, kDspNodes, "DSP node"));
    p.gainDb = value<double>(j, "gainDb");
    p.volumeDb = value<double>(j, "volumeDb");
    p.muted = value<bool>(j, "muted");
    p.crossfadeSeconds = value<double>(j, "crossfadeSeconds");
    p.fadeSkip = enumFrom(field(j, "fadeSkip"), kFadeSkips, "crossfade skip");
    p.state = enumFrom(field(j, "state"), kPipelineStates, "pipeline state");
    return p;
}

template <class T> json optionalJson(const std::optional<T>& v) {
    return v ? toJson(*v) : json(nullptr);
}

json toJson(const PlaybackSnapshot& p) {
    json receivers = json::array();
    for (const auto& r : p.receivers)
        receivers.push_back({{"id", r.id},
                             {"name", r.name},
                             {"level", r.level},
                             {"muted", r.muted},
                             {"channels", nameOf(r.channels, kAirPlayChannels)}});
    const json left = p.receiverLeft ? json{{"serial", p.receiverLeft->serial},
                                            {"name", p.receiverLeft->name},
                                            {"code", nameOf(p.receiverLeft->code, kErrorCodes)},
                                            {"detail", p.receiverLeft->detail}}
                                     : json(nullptr);
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
            {"bufferingSeconds", p.bufferingSeconds ? json(*p.bufferingSeconds) : json(nullptr)},
            {"waitingForPlayer", p.waitingForPlayer},
            {"playerVolume", p.playerVolume ? json(*p.playerVolume) : json(nullptr)},
            {"playerVolumeDb", p.playerVolumeDb ? json(*p.playerVolumeDb) : json(nullptr)},
            {"playerMuted", p.playerMuted},
            {"receivers", receivers},
            {"receiverLeft", left},
            {"receiverAsking", p.receiverAsking},
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
    p.bufferingSeconds =
        optional<uint32_t>(j, "bufferingSeconds", [](const json& v) { return unsignedFrom<uint32_t>(v, "bufferingSeconds"); });
    p.waitingForPlayer = j.contains("waitingForPlayer") && value<bool>(j, "waitingForPlayer");
    p.playerVolume =
        optional<uint32_t>(j, "playerVolume", [](const json& v) { return unsignedFrom<uint32_t>(v, "playerVolume"); });
    if (j.contains("playerVolumeDb") && !j["playerVolumeDb"].is_null()) p.playerVolumeDb = value<double>(j, "playerVolumeDb");
    p.playerMuted = value<bool>(j, "playerMuted");
    if (j.contains("receivers"))
        for (const auto& r : array(j, "receivers"))
            p.receivers.push_back({value<std::string>(r, "id"), value<std::string>(r, "name"), value<uint32_t>(r, "level"),
                                   value<bool>(r, "muted"), enumFrom(field(r, "channels"), kAirPlayChannels, "channels")});
    p.receiverLeft = optional<ReceiverLeft>(j, "receiverLeft", [](const json& v) {
        return ReceiverLeft{value<uint64_t>(v, "serial"), value<std::string>(v, "name"),
                            enumFrom(field(v, "code"), kErrorCodes, "error code"), value<std::string>(v, "detail")};
    });
    if (j.contains("receiverAsking")) p.receiverAsking = value<std::string>(j, "receiverAsking");
    p.error = optional<ErrorCode>(j, "error", [](const json& v) { return enumFrom(v, kErrorCodes, "error code"); });
    p.errorDetail = value<std::string>(j, "errorDetail");
    p.revision = value<uint64_t>(j, "revision");
    return p;
}

json toJson(const std::vector<DeviceInfo>& devices) {
    json out = json::array();
    for (const auto& d : devices)
        out.push_back({{"id", d.id},
                       {"name", d.name},
                       {"unsupported", d.unsupported},
                       {"speakers", d.speakers},
                       {"model", d.model},
                       {"container", d.container},
                       {"mac", d.mac}});
    return out;
}

std::vector<DeviceInfo> devicesFrom(const json& j, const char* key) {
    std::vector<DeviceInfo> out;
    for (const auto& d : array(j, key)) {
        std::vector<std::string> speakers;
        for (const auto& speaker : array(d, "speakers")) {
            if (!speaker.is_string()) invalid("speakers holds a value that is not a string");
            speakers.push_back(speaker.get<std::string>());
        }
        out.push_back({value<std::string>(d, "id"), value<std::string>(d, "name"), value<std::string>(d, "unsupported"),
                       std::move(speakers), value<std::string>(d, "model"), value<std::string>(d, "container"),
                       d.contains("mac") ? value<std::string>(d, "mac") : std::string()});
    }
    return out;
}

json toJson(const QueueSnapshot& q) {
    json entries = json::array();
    for (const auto& e : q.entries)
        entries.push_back({{"id", e.id},
                           {"source", toJson(e.source)},
                           {"title", e.title},
                           {"artist", e.artist},
                           {"album", e.album},
                           {"albumId", e.albumId},
                           {"automatic", e.automatic},
                           {"file", e.file},
                           {"favorite", e.favorite}});
    return {{"revision", q.revision}, {"marks", q.marks},
            {"entries", entries},     {"upcomingEntryId", q.upcomingEntryId ? json(*q.upcomingEntryId) : json(nullptr)},
            {"shuffle", q.shuffle},   {"repeat", nameOf(q.repeat, kRepeatModes)}};
}

QueueSnapshot queueFrom(const json& j) {
    QueueSnapshot q;
    q.revision = value<uint64_t>(j, "revision");
    if (j.contains("marks")) q.marks = value<uint64_t>(j, "marks");
    std::vector<QueueEntry> entries;
    for (const auto& e : array(j, "entries")) {
        QueueEntry entry{value<uint64_t>(e, "id"), trackSourceFrom(field(e, "source"))};
        if (e.contains("title")) entry.title = value<std::string>(e, "title");
        if (e.contains("artist")) entry.artist = value<std::string>(e, "artist");
        if (e.contains("album")) entry.album = value<std::string>(e, "album");
        if (e.contains("albumId")) entry.albumId = value<int64_t>(e, "albumId");
        if (e.contains("automatic")) entry.automatic = value<bool>(e, "automatic");
        if (e.contains("file")) entry.file = value<int64_t>(e, "file");
        if (e.contains("favorite")) entry.favorite = value<bool>(e, "favorite");
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
    json sources = json::array(), tracks = json::array(), artists = json::array();
    for (const auto& s : c.sources) sources.push_back(toJson(s));
    for (const auto& t : c.tracks) tracks.push_back({{"file", t.file}});
    for (const auto& a : c.artists) artists.push_back({{"field", nameOf(a.field, kArtistFields)}, {"name", a.value}});
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
            {"path", c.path},
            {"favorite", c.favorite},
            {"albums", c.albums},
            {"artists", artists},
            {"playerVolume", c.playerVolume ? json(*c.playerVolume) : json(nullptr)},
            {"playerMute", c.playerMute ? json(*c.playerMute) : json(nullptr)},
            {"receiver", c.receiver},
            {"code", c.code},
            {"zone", c.zone},
            {"target", c.target}};
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
    if (j.contains("tracks") || c.kind == CommandKind::ResetPlayStatistics || c.kind == CommandKind::SetFavorite)
        c.tracks = trackKeysFrom(j, "tracks");
    if (j.contains("path") || c.kind == CommandKind::AddLibraryRoot) c.path = value<std::string>(j, "path");
    if (j.contains("favorite")) c.favorite = value<bool>(j, "favorite");
    if (j.contains("albums") || c.kind == CommandKind::SetAlbumFavorite)
        for (const auto& v : array(j, "albums")) {
            if (!v.is_number_integer()) invalid("albums holds a non-integer");
            c.albums.push_back(v.get<int64_t>());
        }
    if (j.contains("artists") || c.kind == CommandKind::SetArtistFavorite)
        for (const auto& a : array(j, "artists"))
            c.artists.push_back({enumFrom(field(a, "field"), kArtistFields, "artist field"), value<std::string>(a, "name")});
    c.playerVolume =
        optional<uint32_t>(j, "playerVolume", [](const json& v) { return unsignedFrom<uint32_t>(v, "playerVolume"); });
    c.playerMute = optional<bool>(j, "playerMute", [](const json& v) {
        if (!v.is_boolean()) invalid("playerMute is not a boolean");
        return v.get<bool>();
    });
    if (j.contains("receiver") || c.kind == CommandKind::SetReceiverVolume) c.receiver = value<std::string>(j, "receiver");
    if (j.contains("code") || c.kind == CommandKind::PairAirPlay) c.code = value<std::string>(j, "code");
    if (j.contains("zone")) c.zone = value<std::string>(j, "zone");
    if (j.contains("target") || c.kind == CommandKind::Transfer) c.target = value<std::string>(j, "target");
    return c;
}

namespace {

json toJson(const ZoneSnapshot& z) {
    return {{"id", z.id},
            {"backend", backendId(z.backend)},
            {"output", z.output},
            {"name", z.name},
            {"queueEntryId", z.queueEntryId},
            {"track", optionalJson(z.track)},
            {"queue", toJson(z.queue)},
            {"playback", toJson(z.playback)},
            {"inUseBy", z.inUseBy}};
}

ZoneSnapshot zoneFrom(const json& j) {
    ZoneSnapshot z;
    z.id = value<std::string>(j, "id");
    z.backend = backendFrom(field(j, "backend"));
    z.output = value<std::string>(j, "output");
    z.name = value<std::string>(j, "name");
    z.queueEntryId = value<uint64_t>(j, "queueEntryId");
    z.track = optional<TrackSource>(j, "track", trackSourceFrom);
    z.queue = queueFrom(field(j, "queue"));
    z.playback = playbackFrom(field(j, "playback"));
    z.inUseBy = value<std::string>(j, "inUseBy");
    return z;
}

const ZoneSnapshot* selectedZone(const Snapshot& s) {
    const auto found = std::ranges::find(s.zones, s.selectedZone, &ZoneSnapshot::id);
    return found == s.zones.end() ? nullptr : &*found;
}

} // namespace

// The selected zone's state is not repeated outside `zones`, as its queue can be long.
json toJson(const Snapshot& s) {
    json zones = json::array();
    for (const auto& z : s.zones) zones.push_back(toJson(z));
    json j = {{"apiVersion", s.apiVersion},
              {"revision", s.revision},
              {"command", toJson(s.command)},
              {"zones", zones},
              {"selectedZone", s.selectedZone},
              {"settings", toJson(s.settings)},
              {"devices",
               {{"revision", s.devices.revision},
                {"wasapi", toJson(s.devices.wasapi)},
                {"asio", toJson(s.devices.asio)},
                {"ks", toJson(s.devices.ks)},
                {"network", toJson(s.devices.network)},
                {"airplay", toJson(s.devices.airplay)},
                {"hqplayer", toJson(s.devices.hqplayer)},
                {"defaultWasapi", s.devices.defaultWasapi}}},
              {"commandError", optionalJson(s.commandError)},
              {"finished", {{"command", s.finished.command}, {"failure", optionalJson(s.finished.failure)}}},
              {"controlPanelOpened", s.controlPanelOpened ? json(*s.controlPanelOpened) : json(nullptr)}};
    const ZoneSnapshot* shown = selectedZone(s);
    if (shown && shown->queueEntryId == s.queueEntryId && shown->track == s.track && shown->queue == s.queue &&
        shown->playback == s.playback)
        return j;
    j["queueEntryId"] = s.queueEntryId;
    j["track"] = optionalJson(s.track);
    j["queue"] = toJson(s.queue);
    j["playback"] = toJson(s.playback);
    return j;
}

Snapshot snapshotFromJson(const json& j) {
    Snapshot s;
    s.apiVersion = value<uint32_t>(j, "apiVersion");
    s.revision = value<uint64_t>(j, "revision");
    s.command = commandStampFromJson(field(j, "command"));
    for (const auto& z : array(j, "zones")) s.zones.push_back(zoneFrom(z));
    s.selectedZone = value<std::string>(j, "selectedZone");
    if (const ZoneSnapshot* shown = selectedZone(s); shown && !j.contains("queue")) {
        s.queueEntryId = shown->queueEntryId;
        s.track = shown->track;
        s.queue = shown->queue;
        s.playback = shown->playback;
    } else {
        s.queueEntryId = value<uint64_t>(j, "queueEntryId");
        s.track = optional<TrackSource>(j, "track", trackSourceFrom);
        s.queue = queueFrom(field(j, "queue"));
        s.playback = playbackFrom(field(j, "playback"));
    }
    s.settings = settingsFrom(field(j, "settings"));
    const json& devices = field(j, "devices");
    s.devices.revision = value<uint64_t>(devices, "revision");
    s.devices.wasapi = devicesFrom(devices, "wasapi");
    s.devices.asio = devicesFrom(devices, "asio");
    s.devices.ks = devicesFrom(devices, "ks");
    s.devices.network = devicesFrom(devices, "network");
    s.devices.airplay = devicesFrom(devices, "airplay");
    s.devices.hqplayer = devicesFrom(devices, "hqplayer");
    s.devices.defaultWasapi = value<std::string>(devices, "defaultWasapi");
    s.commandError = optional<Failure>(j, "commandError", failureFrom);
    const json& finished = field(j, "finished");
    s.finished = {value<uint64_t>(finished, "command"), optional<Failure>(finished, "failure", failureFrom)};
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
