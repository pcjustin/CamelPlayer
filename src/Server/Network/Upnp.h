// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The messages of UPnP AV and OpenHome players (docs/Plan.md §5.10): SSDP, device descriptions, SOAP and GENA. Everything
// here comes from the network and is parsed with its size limited and without DTDs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace camel::server::upnp {

inline constexpr size_t kMaxMessageBytes = size_t{1} << 20;

struct XmlNode {
    std::string name; // without its namespace prefix
    std::string text;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
    const XmlNode* child(std::string_view childName) const;
    std::string childText(std::string_view childName) const;
};
std::optional<XmlNode> parseXml(std::string_view xml);

enum class PlayerProtocol : uint8_t { UpnpAv, OpenHome };

struct PlayerService {
    std::string type, controlUrl, eventUrl, scpdUrl;
    bool operator==(const PlayerService&) const = default;
};

struct PlayerDescription {
    std::string udn, name, manufacturer, model;
    PlayerProtocol protocol = PlayerProtocol::UpnpAv;
    std::vector<PlayerService> services;
    const PlayerService* service(std::string_view kind) const; // "AVTransport", "Playlist", ...
    bool operator==(const PlayerDescription&) const = default;
};

// A player is told apart by what it offers: OpenHome when it has Product and Playlist, otherwise UPnP AV when it has
// AVTransport; nothing for anything else. Relative URLs resolve against `location`; an absolute one on another host is
// not used.
std::optional<PlayerDescription> parseDescription(std::string_view xml, const std::string& location);

enum class SsdpKind : uint8_t { Response, Alive, ByeBye };

struct SsdpMessage {
    SsdpKind kind = SsdpKind::Response;
    std::string udn, location;
    bool operator==(const SsdpMessage&) const = default;
};
// Nothing for messages about anything but a media renderer or an OpenHome product.
std::optional<SsdpMessage> parseSsdp(std::string_view text);
std::vector<std::string> searchRequests(const std::string& host, uint16_t port); // one M-SEARCH for each kind of player

// Only plain HTTP to an address on the local network: a message on the network must not make CamelPlayer fetch anything
// else.
bool isLanUrl(const std::string& url);
std::string hostOf(const std::string& url);

std::string soapRequest(std::string_view serviceType, std::string_view action,
                        const std::vector<std::pair<std::string, std::string>>& arguments);

struct SoapReply {
    bool ok = false;
    std::map<std::string, std::string> values;
    int errorCode = 0; // the UPnP error of a fault
};
SoapReply parseSoapReply(unsigned status, std::string_view body);

// The variables of a GENA event, and those inside a LastChange value.
std::map<std::string, std::string> parsePropertySet(std::string_view xml);
std::map<std::string, std::string> parseLastChange(std::string_view xml);

// What a player's ConnectionManager declares it can play.
struct PlayerFormats {
    bool wav = false, l16 = false, l24 = false, dsf = false;
    bool operator==(const PlayerFormats&) const = default;
};
PlayerFormats parseProtocolInfo(std::string_view sink);
std::vector<std::string> parseActions(std::string_view scpd);

struct DidlTrack {
    std::string title, artist, album, coverUrl, protocolInfo, url;
    uint64_t durationMs = 0, sizeBytes = 0;
};
std::string didlLite(const DidlTrack& track);

// The address of this computer that `host` can reach.
std::string localAddressFor(const std::string& host);

std::optional<uint64_t> parseDuration(std::string_view text); // h:mm:ss[.fff] in milliseconds
std::string formatDuration(uint64_t milliseconds);

} // namespace camel::server::upnp
