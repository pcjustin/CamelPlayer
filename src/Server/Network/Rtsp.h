// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// An RTSP connection to an AirPlay receiver (docs/Plan.md §5.10). Throws std::runtime_error when the receiver does not
// answer.
#pragma once

#include <winsock2.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace camel::server::airplay {

struct RtspReply {
    int status = 0;
    std::map<std::string, std::string> headers; // names in lower case
    std::string body;
};

// The receiver closed the connection, as opposed to not answering.
class RtspClosed : public std::runtime_error {
public:
    RtspClosed() : std::runtime_error("the receiver closed the connection") {}
};

class RtspClient {
public:
    RtspClient(const std::string& address, uint16_t port, int timeoutMs);
    ~RtspClient();
    RtspClient(const RtspClient&) = delete;
    RtspClient& operator=(const RtspClient&) = delete;

    const std::string& localAddress() const { return localAddress_; }
    const std::string& session() const { return session_; }
    std::string url() const; // of this session
    void setSession(std::string session) { sessionHeader_ = std::move(session); }
    bool closed() const; // whether the receiver has closed the connection, without waiting

    // `headers` are complete lines.
    RtspReply request(std::string_view method, std::string_view url, std::string_view contentType = {},
                      std::string_view body = {}, std::string_view headers = {});

private:
    SOCKET socket_ = INVALID_SOCKET;
    std::string localAddress_, session_, instance_, sessionHeader_;
    int sequence_ = 0;
};

} // namespace camel::server::airplay
