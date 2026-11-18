// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlay2.h"

#include <cctype>
#include <format>

#include "Server/Network/AirPlay.h"
#include "Server/Network/Pairing.h"
#include "Server/Network/Rtsp.h"

namespace camel::server::airplay {

namespace {

constexpr int64_t kRealtimeStream = 96;
constexpr int64_t kAlac = 2;
constexpr int64_t kAlac44100x16x2 = 0x40000;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kNonceBytes = 8;

// "02:A6:06:38:47:4C", the same six bytes as the device ID requests carry.
std::string senderAddress() {
    const std::string& id = senderDeviceId();
    std::string out;
    for (size_t i = 2; i + 1 < id.size(); i += 2) {
        if (!out.empty()) out += ':';
        out += char(std::toupper(static_cast<unsigned char>(id[i])));
        out += char(std::toupper(static_cast<unsigned char>(id[i + 1])));
    }
    return out;
}

std::string littleEndian64(uint64_t value) {
    std::string out;
    for (size_t i = 0; i < kNonceBytes; ++i) out.push_back(char((value >> (8 * i)) & 0xFF));
    return out;
}

} // namespace

std::string randomUuid() {
    const std::string r = randomBytes(16);
    std::string h;
    for (const char c : r) h += std::format("{:02X}", uint8_t(c));
    return std::format("{}-{}-{}-{}-{}", h.substr(0, 8), h.substr(8, 4), h.substr(12, 4), h.substr(16, 4), h.substr(20));
}

Plist sessionSetup(uint16_t timingPort, std::string_view groupUuid) {
    const std::string address = senderAddress();
    Plist setup = Plist::dict();
    setup.set("deviceID", Plist::of(std::string_view(address))).set("macAddress", Plist::of(std::string_view(address)));
    setup.set("sessionUUID", Plist::of(std::string_view(randomUuid()))).set("name", Plist::of("CamelPlayer"));
    setup.set("model", Plist::of("Windows")).set("osName", Plist::of("Windows")).set("osVersion", Plist::of("10.0"));
    setup.set("sourceVersion", Plist::of("999.0.0")).set("timingProtocol", Plist::of("NTP"));
    setup.set("timingPort", Plist::of(int64_t{timingPort})).set("isMultiSelectAirPlay", Plist::of(true));
    setup.set("groupContainsGroupLeader", Plist::of(false)).set("groupUUID", Plist::of(groupUuid));
    setup.set("senderSupportsRelay", Plist::of(false)).set("statsCollectionEnabled", Plist::of(false));
    return setup;
}

Plist streamSetup(std::string_view key, uint16_t controlPort, int64_t connectionId) {
    Plist stream = Plist::dict();
    stream.set("type", Plist::of(kRealtimeStream)).set("ct", Plist::of(kAlac)).set("audioFormat", Plist::of(kAlac44100x16x2));
    stream.set("spf", Plist::of(int64_t{kFramesPerPacket})).set("sr", Plist::of(int64_t{kSampleRate}));
    stream.set("shk", Plist::data(key)).set("controlPort", Plist::of(int64_t{controlPort}));
    stream.set("latencyMin", Plist::of(int64_t{kDefaultAudioLatency})).set("latencyMax", Plist::of(int64_t{88200}));
    stream.set("isMedia", Plist::of(true)).set("audioMode", Plist::of("default"));
    stream.set("streamConnectionID", Plist::of(connectionId)).set("supportsDynamicStreamID", Plist::of(false));
    Plist setup = Plist::dict();
    setup.set("streams", Plist::array({stream}));
    return setup;
}

namespace {

std::optional<uint16_t> portIn(const Plist& dict, std::string_view key) {
    const Plist* value = dict.find(key);
    if (!value || value->kind != Plist::Kind::Integer || value->integer <= 0 || value->integer > 0xFFFF) return std::nullopt;
    return uint16_t(value->integer);
}

} // namespace

std::optional<StreamPorts> streamPortsFrom(const Plist& answer) {
    const Plist* streams = answer.find("streams");
    if (!streams || streams->kind != Plist::Kind::Array || streams->items.empty()) return std::nullopt;
    const Plist& stream = streams->items.front();
    const auto data = portIn(stream, "dataPort");
    if (!data) return std::nullopt;
    return StreamPorts{*data, portIn(stream, "controlPort").value_or(*data)};
}

std::optional<uint16_t> eventPortFrom(const Plist& answer) {
    return portIn(answer, "eventPort");
}

std::string sealedRtpPacket(std::string_view rtp, std::string_view key, uint64_t nonce) {
    const std::string tail = littleEndian64(nonce);
    const std::string sealed =
        seal(key, std::string(4, '\0') + tail, rtp.substr(4, kHeaderBytes - 4), rtp.substr(std::min(rtp.size(), kHeaderBytes)));
    return std::string(rtp.substr(0, kHeaderBytes)) + sealed + tail;
}

} // namespace camel::server::airplay
