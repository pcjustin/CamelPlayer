// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// An RTSP connection to an AirPlay receiver (docs/Plan.md §5.10). Throws std::runtime_error when the receiver does not
// answer.
#pragma once

#include <winsock2.h>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Server/Network/Pairing.h"

namespace camel::server::airplay {

struct RtspReply {
    int status = 0;
    std::map<std::string, std::string> headers; // names in lower case
    std::string body;
};

RtspReply parseRtspHead(std::string_view head); // a reply's status line and headers

// How long a request waits that a Mac answers only once its user accepts CamelPlayer: it waits 15 s for the answer.
inline constexpr int kUserAnswerTimeoutMs = 20000;

// "0x" and twelve hex digits, the same on every connection from this computer, so that a receiver that remembers whom its
// user allowed knows CamelPlayer again.
const std::string& senderDeviceId();

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
    void setTimeout(int timeoutMs); // for each request from now on
    bool closed() const;            // whether the receiver has closed the connection, without waiting

    // `headers` are complete lines.
    RtspReply request(std::string_view method, std::string_view url, std::string_view contentType = {},
                      std::string_view body = {}, std::string_view headers = {});

    // Transient pair-setup, with the code every such receiver takes or the password a receiver asks for; once it
    // succeeds the connection is encrypted.
    PairOutcome pairTransiently(std::string_view pin = kTransientPin);
    // Makes a receiver that pairs with a code show it, once its user accepts CamelPlayer if it asks.
    bool showPin(int userTimeoutMs = kUserAnswerTimeoutMs);
    // Pair-setup with a code, which leaves the receiver's long-term key in `receiver`; a session still needs pair-verify.
    PairOutcome pairSetup(std::string_view pin, const PairingIdentity& me, PairedReceiver& receiver);
    // With a receiver paired before; once it succeeds the connection is encrypted. False when it no longer knows this
    // computer.
    bool pairVerify(const PairingIdentity& me, const PairedReceiver& receiver);

private:
    SOCKET socket_ = INVALID_SOCKET;
    std::string localAddress_, session_, instance_, sessionHeader_;
    std::map<uint8_t, std::string> pairingStep(std::string_view path, std::string_view headers, const TlvItems& items,
                                               bool& answered);
    std::optional<SrpProof> exchangeProofs(std::string_view pin, bool transient, PairOutcome& failure);

    int sequence_ = 0;
    int timeoutMs_ = 0;
    std::optional<SecureChannel> channel_;
};

} // namespace camel::server::airplay
