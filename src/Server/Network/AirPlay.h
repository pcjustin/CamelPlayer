// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// AirPlay 1 receivers as mDNS describes them, written from public descriptions of the protocol (docs/Plan.md §5.10).
// Only unencrypted sessions: nothing here answers a challenge or holds a key.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace camel::server::airplay {

enum class DnsType : uint16_t { A = 1, Ptr = 12, Txt = 16, Srv = 33 };
using DnsName = std::vector<std::string>; // labels, as a label can hold a dot
using TxtRecord = std::map<std::string, std::string>;

struct DnsQuestion {
    DnsName name;
    DnsType type = DnsType::Ptr;
};

struct DnsRecord {
    DnsType type = DnsType::A;
    DnsName name;
    DnsName target;      // of a PTR or an SRV
    uint16_t port = 0;   // of an SRV
    TxtRecord txt;       // keys in lower case
    std::string address; // of an A
};

DnsName serviceName(std::string_view service); // from "_raop._tcp"
// Questions asking for answers to the asker's own port, or with `unicast` false for answers to the group.
std::string queryMessage(const std::vector<DnsQuestion>& questions, bool unicast = true);
// Every answer and additional record of a known type, up to any damage to the message.
std::vector<DnsRecord> parseRecords(std::string_view message);

// "32FBAAAB399D@Name", the name of a RAOP service, as the device ID and the receiver's name.
std::optional<std::pair<std::string, std::string>> parseRaopName(std::string_view label);
std::string deviceIdFrom(std::string_view text); // upper case, without separators

// HomeOnly: its access setting allows only people sharing its home (`acl=1` in _airplay._tcp), which this computer is not.
// Password and Code: it takes a session once paired with the password set on it or the code it shows.
enum class Support { Supported, EncryptedOnly, Password, Format, Refused, HomeOnly, Code };
const char* supportId(Support support);     // empty for Supported
Support supportFrom(const TxtRecord& raop); // what the receiver declares; only a session shows whether it holds

// Why a session failed when the receiver's user declined CamelPlayer or did not answer; the Control point tells it apart.
inline constexpr std::string_view kNotAllowedOnReceiver = "the receiver's user did not allow CamelPlayer";

struct Announced {
    std::string deviceId, name, address;
    uint16_t port = 0;
    TxtRecord raop;
    TxtRecord airplay; // empty for a receiver without an _airplay._tcp service
};

struct Speaker {
    std::string address;
    uint16_t port = 0;
    bool operator==(const Speaker&) const = default;
};

struct Receiver {
    std::string id; // the device ID, or the group of a stereo pair, which either speaker may lead
    std::string name, address;
    uint16_t port = 0;
    std::string version;
    Support support = Support::Supported;
    std::vector<Speaker> others;       // the rest of a stereo pair, which does not pass the stream on
    std::vector<std::string> speakers; // a stereo pair's members by device ID, sorted; empty for one receiver
    bool transient = false;            // takes a session only after transient pairing
    bool asksPassword = false;         // its flags say a password is set on it
    bool asksCode = false;             // its flags say it pairs once with a code it shows
    bool operator==(const Receiver&) const = default;
};

// One receiver for each Apple device or stereo pair, by name. A pair is asked through the member that leads it.
std::vector<Receiver> receiversFrom(const std::vector<Announced>& announced);

// The ANNOUNCE body for 44.1 kHz/16-bit stereo as ALAC frames of 352 samples, without keys.
std::string sessionDescription(std::string_view localAddress, std::string_view remoteAddress, std::string_view session);

constexpr uint32_t kSampleRate = 44100;
constexpr uint64_t kFramesPerPacket = 352;
constexpr uint32_t kSyncLatency = 77175;         // between the frame a sync packet says is playing and the newest sent
constexpr uint32_t kDefaultAudioLatency = 11025; // added by a receiver that does not say
constexpr double kMutedDb = -144.0;

// `pcm` holds 352 little-endian 16-bit stereo frames; the ALAC frame carries them unchanged, uncompressed.
std::string alacFrame(const uint8_t* pcm);
std::string rtpPacket(uint16_t sequence, uint32_t rtpTime, uint32_t ssrc, bool first, std::string_view alac);
std::string syncPacket(bool first, uint32_t playing, uint64_t ntpTime, uint32_t newest);
std::string timingReply(std::string_view request, uint64_t received, uint64_t sent); // empty for anything else

struct ResendRequest {
    uint16_t first = 0, count = 0;
};
std::optional<ResendRequest> parseResend(std::string_view message);
std::string resendPacket(uint16_t sequence, std::string_view rtp);

std::string dmapTags(std::string_view title, std::string_view artist, std::string_view album);
double volumeDb(uint32_t level, bool muted); // from the slider's 0-100 to the receiver's -30-0 dB, 0 being silence

} // namespace camel::server::airplay
