// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Upnp.h"

#include <objbase.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xmllite.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

#include "Common/util/Text.h"

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace camel::server::upnp {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '"')
            out += "&quot;";
        else
            out += c;
    }
    return out;
}

// "urn:schemas-upnp-org:service:AVTransport:1" is the kind "AVTransport".
std::string kindOf(std::string_view serviceType) {
    const auto end = serviceType.rfind(':');
    if (end == std::string_view::npos || end == 0) return {};
    const auto start = serviceType.rfind(':', end - 1);
    return start == std::string_view::npos ? std::string() : std::string(serviceType.substr(start + 1, end - start - 1));
}

std::string originOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    return url.substr(0, url.find('/', scheme + 3));
}

std::string resolve(const std::string& location, const std::string& reference) {
    if (reference.empty()) return {};
    if (reference.find("://") != std::string::npos)
        return lower(originOf(reference)) == lower(originOf(location)) ? reference : std::string();
    if (reference.front() == '/') return originOf(location) + reference;
    return location.substr(0, location.rfind('/') + 1) + reference;
}

const XmlNode* findRenderer(const XmlNode& device) {
    if (const auto* services = device.child("serviceList"))
        for (const auto& service : services->children) {
            const auto kind = kindOf(service.childText("serviceType"));
            if (kind == "AVTransport" || kind == "Product") return &device;
        }
    if (const auto* list = device.child("deviceList"))
        for (const auto& embedded : list->children)
            if (const auto* found = findRenderer(embedded)) return found;
    return nullptr;
}

std::optional<std::map<std::string, std::string>> headersOf(std::string_view text) {
    std::map<std::string, std::string> headers;
    size_t start = text.find("\r\n");
    if (start == std::string_view::npos) return std::nullopt;
    start += 2;
    while (start < text.size()) {
        auto end = text.find("\r\n", start);
        if (end == std::string_view::npos) end = text.size();
        const auto line = text.substr(start, end - start);
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon != std::string_view::npos)
            headers[lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
        start = end + 2;
    }
    return headers;
}

} // namespace

const XmlNode* XmlNode::child(std::string_view childName) const {
    const auto found = std::ranges::find_if(children, [&](const XmlNode& node) { return node.name == childName; });
    return found == children.end() ? nullptr : &*found;
}

std::string XmlNode::childText(std::string_view childName) const {
    const XmlNode* node = child(childName);
    return node ? node->text : std::string();
}

std::optional<XmlNode> parseXml(std::string_view xml) {
    if (xml.empty() || xml.size() > kMaxMessageBytes) return std::nullopt;
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()), UINT(xml.size())));
    wil::com_ptr<IXmlReader> reader;
    if (!stream || FAILED(CreateXmlReader(IID_PPV_ARGS(&reader), nullptr)) || FAILED(reader->SetInput(stream.get())))
        return std::nullopt;
    XmlNode root;
    std::vector<XmlNode*> open{&root};
    XmlNodeType type{};
    HRESULT result = S_OK;
    while ((result = reader->Read(&type)) == S_OK) {
        const wchar_t* value = nullptr;
        if (type == XmlNodeType_Element) {
            if (open.size() > 64) return std::nullopt;
            XmlNode node;
            if (FAILED(reader->GetLocalName(&value, nullptr))) return std::nullopt;
            node.name = toUtf8(value);
            const bool empty = reader->IsEmptyElement() != FALSE;
            for (HRESULT more = reader->MoveToFirstAttribute(); more == S_OK; more = reader->MoveToNextAttribute()) {
                const wchar_t* name = nullptr;
                if (FAILED(reader->GetLocalName(&name, nullptr)) || FAILED(reader->GetValue(&value, nullptr)))
                    return std::nullopt;
                node.attributes[toUtf8(name)] = toUtf8(value);
            }
            open.back()->children.push_back(std::move(node));
            if (!empty) open.push_back(&open.back()->children.back());
        } else if (type == XmlNodeType_EndElement) {
            if (open.size() <= 1) return std::nullopt;
            open.pop_back();
        } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            if (FAILED(reader->GetValue(&value, nullptr))) return std::nullopt;
            open.back()->text += toUtf8(value);
        }
    }
    if (FAILED(result) || root.children.empty()) return std::nullopt;
    return std::move(root.children.front());
}

const PlayerService* PlayerDescription::service(std::string_view kind) const {
    const auto found = std::ranges::find_if(services, [&](const PlayerService& s) { return kindOf(s.type) == kind; });
    return found == services.end() ? nullptr : &*found;
}

std::optional<PlayerDescription> parseDescription(std::string_view xml, const std::string& location) {
    const auto root = parseXml(xml);
    if (!root || root->name != "root" || !root->child("device")) return std::nullopt;
    const XmlNode* device = findRenderer(*root->child("device"));
    if (!device) return std::nullopt;
    PlayerDescription player;
    player.udn = device->childText("UDN");
    player.name = device->childText("friendlyName");
    player.manufacturer = device->childText("manufacturer");
    player.model = device->childText("modelName");
    if (player.udn.empty()) return std::nullopt;
    for (const auto& service : device->child("serviceList")->children) {
        PlayerService entry;
        entry.type = service.childText("serviceType");
        entry.controlUrl = resolve(location, service.childText("controlURL"));
        entry.eventUrl = resolve(location, service.childText("eventSubURL"));
        entry.scpdUrl = resolve(location, service.childText("SCPDURL"));
        if (!entry.type.empty() && !entry.controlUrl.empty()) player.services.push_back(std::move(entry));
    }
    if (player.service("Product") && player.service("Playlist"))
        player.protocol = PlayerProtocol::OpenHome;
    else if (player.service("AVTransport"))
        player.protocol = PlayerProtocol::UpnpAv;
    else
        return std::nullopt;
    return player;
}

std::optional<SsdpMessage> parseSsdp(std::string_view text) {
    if (text.size() > kMaxMessageBytes) return std::nullopt;
    const auto headers = headersOf(text);
    if (!headers) return std::nullopt;
    const auto value = [&](const char* name) {
        const auto found = headers->find(name);
        return found == headers->end() ? std::string() : found->second;
    };
    SsdpMessage message;
    std::string target;
    if (text.starts_with("HTTP/1.1 200")) {
        target = value("st");
    } else if (text.starts_with("NOTIFY")) {
        target = value("nt");
        const auto subtype = value("nts");
        if (subtype == "ssdp:alive")
            message.kind = SsdpKind::Alive;
        else if (subtype == "ssdp:byebye")
            message.kind = SsdpKind::ByeBye;
        else
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (target.find(":device:MediaRenderer:") == std::string::npos && target.find(":service:Product:") == std::string::npos)
        return std::nullopt;
    const auto usn = value("usn");
    message.udn = usn.substr(0, usn.find("::"));
    message.location = value("location");
    if (message.udn.empty() || (message.kind != SsdpKind::ByeBye && !isLanUrl(message.location))) return std::nullopt;
    return message;
}

std::vector<std::string> searchRequests(const std::string& host, uint16_t port) {
    std::vector<std::string> requests;
    for (const char* target : {"urn:schemas-upnp-org:device:MediaRenderer:1", "urn:av-openhome-org:service:Product:1"})
        requests.push_back("M-SEARCH * HTTP/1.1\r\nHOST: " + host + ":" + std::to_string(port) +
                           "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + target + "\r\n\r\n");
    return requests;
}

std::string hostOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const auto start = scheme + 3;
    const auto end = url.find_first_of(":/", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool isLanUrl(const std::string& url) {
    if (!url.starts_with("http://")) return false;
    in_addr address{};
    if (inet_pton(AF_INET, hostOf(url).c_str(), &address) != 1) return false;
    const uint32_t ip = ntohl(address.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}

std::string soapRequest(std::string_view serviceType, std::string_view action,
                        const std::vector<std::pair<std::string, std::string>>& arguments) {
    std::string body = R"(<?xml version="1.0" encoding="utf-8"?>)"
                       R"(<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" )"
                       R"(s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>)";
    body += "<u:" + std::string(action) + " xmlns:u=\"" + escape(serviceType) + "\">";
    for (const auto& [name, value] : arguments) {
        body += "<" + name + ">";
        body += escape(value);
        body += "</" + name + ">";
    }
    body += "</u:" + std::string(action) + "></s:Body></s:Envelope>";
    return body;
}

SoapReply parseSoapReply(unsigned status, std::string_view body) {
    SoapReply reply;
    const auto root = parseXml(body);
    const XmlNode* soapBody = root ? root->child("Body") : nullptr;
    if (!soapBody || soapBody->children.empty()) return reply;
    const XmlNode& first = soapBody->children.front();
    if (first.name == "Fault") {
        if (const auto* detail = first.child("detail"))
            for (const auto& item : detail->children) {
                const auto code = item.childText("errorCode");
                std::from_chars(code.data(), code.data() + code.size(), reply.errorCode);
            }
        return reply;
    }
    reply.ok = status == 200;
    for (const auto& value : first.children) reply.values[value.name] = value.text;
    return reply;
}

std::map<std::string, std::string> parsePropertySet(std::string_view xml) {
    std::map<std::string, std::string> values;
    if (const auto root = parseXml(xml))
        for (const auto& property : root->children)
            for (const auto& variable : property.children) values[variable.name] = variable.text;
    return values;
}

std::map<std::string, std::string> parseLastChange(std::string_view xml) {
    std::map<std::string, std::string> values;
    const auto root = parseXml(xml);
    if (!root) return values;
    const XmlNode* instance = root->child("InstanceID");
    if (!instance) return values;
    for (const auto& variable : instance->children) {
        // The DMP-A6 spells the attribute "Channel".
        const bool otherChannel = std::ranges::any_of(variable.attributes, [](const auto& attribute) {
            return _stricmp(attribute.first.c_str(), "channel") == 0 && _stricmp(attribute.second.c_str(), "Master") != 0;
        });
        if (otherChannel) continue;
        const auto value = variable.attributes.find("val");
        if (value != variable.attributes.end()) values[variable.name] = value->second;
    }
    return values;
}

PlayerFormats parseProtocolInfo(std::string_view sink) {
    PlayerFormats formats;
    size_t start = 0;
    while (start <= sink.size()) {
        auto end = sink.find(',', start);
        if (end == std::string_view::npos) end = sink.size();
        const std::string entry = lower(trim(sink.substr(start, end - start)));
        start = end + 1;
        if (!entry.starts_with("http-get:")) continue;
        const auto contentType = entry.find(':', 9);
        if (contentType == std::string::npos) continue;
        const std::string mime = entry.substr(contentType + 1, entry.find(':', contentType + 1) - contentType - 1);
        if (mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/wave" || mime == "audio/vnd.wave") formats.wav = true;
        if (mime.starts_with("audio/l16")) formats.l16 = true;
        if (mime.starts_with("audio/l24")) formats.l24 = true;
        if (mime == "audio/dsf" || mime == "audio/x-dsf" || mime == "audio/dsd") formats.dsf = true;
    }
    return formats;
}

std::vector<std::string> parseActions(std::string_view scpd) {
    std::vector<std::string> actions;
    const auto root = parseXml(scpd);
    if (!root) return actions;
    if (const auto* list = root->child("actionList"))
        for (const auto& action : list->children) actions.push_back(action.childText("name"));
    return actions;
}

std::string didlLite(const DidlTrack& track) {
    std::string item =
        R"(<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" )"
        R"(xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"><item id="1" parentID="0" restricted="1">)";
    item += "<dc:title>" + escape(track.title) + "</dc:title>";
    if (!track.artist.empty())
        item += "<dc:creator>" + escape(track.artist) + "</dc:creator><upnp:artist>" + escape(track.artist) + "</upnp:artist>";
    if (!track.album.empty()) item += "<upnp:album>" + escape(track.album) + "</upnp:album>";
    if (!track.coverUrl.empty()) item += "<upnp:albumArtURI>" + escape(track.coverUrl) + "</upnp:albumArtURI>";
    item += "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    item += "<res protocolInfo=\"" + escape(track.protocolInfo) + "\"";
    if (track.durationMs) item += " duration=\"" + formatDuration(track.durationMs) + ".000\"";
    if (track.sizeBytes) item += " size=\"" + std::to_string(track.sizeBytes) + "\"";
    item += ">" + escape(track.url) + "</res></item></DIDL-Lite>";
    return item;
}

std::string localAddressFor(const std::string& host) {
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    inet_pton(AF_INET, host.c_str(), &target.sin_addr);
    std::string local;
    if (connect(probe, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in address{};
        int length = sizeof(address);
        char text[INET_ADDRSTRLEN] = {};
        if (getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)))
            local = text;
    }
    closesocket(probe);
    return local;
}

std::optional<uint64_t> parseDuration(std::string_view text) {
    unsigned hours = 0, minutes = 0, seconds = 0, fraction = 0;
    int consumed = 0;
    const std::string copy(trim(text));
    if (sscanf_s(copy.c_str(), "%u:%u:%u%n", &hours, &minutes, &seconds, &consumed) != 3) return std::nullopt;
    const std::string_view rest = std::string_view(copy).substr(size_t(consumed));
    if (rest.starts_with('.')) {
        const auto digits = rest.substr(1, 3);
        std::from_chars(digits.data(), digits.data() + digits.size(), fraction);
        for (size_t i = digits.size(); i < 3; ++i) fraction *= 10;
    }
    return (uint64_t{hours} * 3600 + uint64_t{minutes} * 60 + seconds) * 1000 + fraction;
}

std::string formatDuration(uint64_t milliseconds) {
    const uint64_t seconds = milliseconds / 1000;
    char text[32];
    snprintf(text, sizeof(text), "%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return text;
}

} // namespace camel::server::upnp
