// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a device on the network can send: UPnP, SSDP, mDNS and RTSP, and the Server API's WebSocket frames, HTTP heads,
// discovery and messages.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "Server/Api/ApiCalls.h"
#include "Server/Api/ApiJson.h"
#include "Server/Network/AirPlay.h"
#include "Server/Network/Rtsp.h"
#include "Server/Network/Upnp.h"
#include "Server/Remote/Discovery.h"
#include "Server/Remote/WebSocket.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace camel::server;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    upnp::parseXml(text);
    upnp::parseDescription(text, "http://192.0.2.1:49152/description.xml");
    upnp::parseSsdp(text);
    upnp::parseSoapReply(200, text);
    upnp::parseSoapReply(500, text);
    upnp::parsePropertySet(text);
    upnp::parseLastChange(text);
    upnp::parseProtocolInfo(text);
    upnp::parseActions(text);
    upnp::parseDuration(text);
    airplay::parseRecords(text);
    airplay::parseRaopName(text);
    airplay::parseResend(text);
    airplay::timingReply(text, 1, 2);
    try {
        airplay::parseRtspHead(text);
    } catch (const std::runtime_error&) {
        // what a reply that is not RTSP is meant to give
    }
    discovery::asksFor(text, "host");
    discovery::serversIn({std::string(text)});
    for (const bool masked : {false, true}) {
        try {
            websocket::decodeFrame(text, masked, 1 << 20);
        } catch (const std::invalid_argument&) {
            // a frame that breaks RFC 6455
        }
    }
    try {
        websocket::parseHead(text);
        websocket::queryOf(text);
    } catch (const std::invalid_argument&) {
        // not HTTP
    }
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return 0;
    }
    const auto quietly = [](auto&& read) {
        try {
            read();
        } catch (const nlohmann::json::exception&) {
        } catch (const std::invalid_argument&) {
        } catch (const std::logic_error&) {
        } catch (const std::runtime_error&) {
            // what a message that is not the API's is meant to give
        }
    };
    quietly([message]() mutable { api::QueueDecoder().decode(message); });
    quietly([&message] { commandFromJson(message); });
    quietly([&message] { snapshotFromJson(message); });
    quietly([&message] { api::rethrow(message); });
    return 0;
}
