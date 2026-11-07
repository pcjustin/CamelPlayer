// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a device on the network can send: UPnP, SSDP, mDNS and RTSP.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "Server/Network/AirPlay.h"
#include "Server/Network/Rtsp.h"
#include "Server/Network/Upnp.h"

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
    return 0;
}
