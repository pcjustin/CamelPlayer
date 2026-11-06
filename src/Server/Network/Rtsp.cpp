// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Rtsp.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <random>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")

namespace camel::server::airplay {

namespace {

constexpr size_t kMaxReplyBytes = 65536;
constexpr std::string_view kEndOfHeaders = "\r\n\r\n";
constexpr std::string_view kEndOfLine = "\r\n";

int numberIn(std::string_view text) {
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

void waitForConnection(SOCKET socket, int timeoutMs) {
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);
    const timeval wait{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &writable, &failed, &wait) <= 0 || FD_ISSET(socket, &failed))
        throw std::runtime_error("the receiver does not accept a connection");
}

RtspReply parseHead(std::string_view head) {
    RtspReply reply;
    const size_t firstLineEnd = head.find(kEndOfLine);
    const std::string_view status = head.substr(0, firstLineEnd);
    const size_t space = status.find(' ');
    if (!status.starts_with("RTSP/") || space == std::string_view::npos) throw std::runtime_error("not an RTSP reply");
    reply.status = numberIn(status.substr(space + 1));
    std::string_view rest = firstLineEnd == std::string_view::npos ? std::string_view{} : head.substr(firstLineEnd + 2);
    while (!rest.empty()) {
        const size_t end = rest.find(kEndOfLine);
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 2);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string name(line.substr(0, colon));
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::string_view value = line.substr(colon + 1);
        value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
        reply.headers[std::move(name)] = value;
    }
    return reply;
}

} // namespace

RtspClient::RtspClient(const std::string& address, uint16_t port, int timeoutMs) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    try {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) throw std::runtime_error("not an address");
        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
        if (connect(socket_, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0 &&
            WSAGetLastError() != WSAEWOULDBLOCK)
            throw std::runtime_error("the receiver does not accept a connection");
        waitForConnection(socket_, timeoutMs);
    } catch (...) {
        closesocket(socket_);
        WSACleanup();
        throw;
    }
    u_long blocking = 0;
    ioctlsocket(socket_, FIONBIO, &blocking);
    const auto timeout = DWORD(timeoutMs);
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in local{};
    int length = sizeof(local);
    getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length);
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
    localAddress_ = text;

    std::random_device random;
    session_ = std::to_string(random());
    instance_ = std::format("{:08X}{:08X}", random(), random());
}

RtspClient::~RtspClient() {
    closesocket(socket_);
    WSACleanup();
}

std::string RtspClient::url() const {
    return std::format("rtsp://{}/{}", localAddress_, session_);
}

bool RtspClient::closed() const {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket_, &readable);
    const timeval now{0, 0};
    if (select(0, &readable, nullptr, nullptr, &now) <= 0) return false;
    char next = 0;
    return recv(socket_, &next, 1, MSG_PEEK) <= 0;
}

RtspReply RtspClient::request(std::string_view method, std::string_view url, std::string_view contentType, std::string_view body,
                              std::string_view headers) {
    std::string message = std::format("{} {} RTSP/1.0\r\nCSeq: {}\r\nUser-Agent: CamelPlayer\r\nClient-Instance: {}\r\n", method,
                                      url, ++sequence_, instance_);
    if (!sessionHeader_.empty()) message += std::format("Session: {}\r\n", sessionHeader_);
    message += headers;
    if (!body.empty()) message += std::format("Content-Type: {}\r\nContent-Length: {}\r\n", contentType, body.size());
    message += kEndOfLine;
    message += body;
    if (send(socket_, message.data(), int(message.size()), 0) != int(message.size())) throw RtspClosed();

    std::string received;
    while (received.size() <= kMaxReplyBytes) {
        const size_t headEnd = received.find(kEndOfHeaders);
        if (headEnd != std::string::npos) {
            RtspReply reply = parseHead(std::string_view(received).substr(0, headEnd));
            const auto length = reply.headers.find("content-length");
            const size_t bodyBytes = length == reply.headers.end() ? 0 : size_t(numberIn(length->second));
            const size_t bodyAt = headEnd + kEndOfHeaders.size();
            if (received.size() >= bodyAt + bodyBytes) {
                reply.body = received.substr(bodyAt, bodyBytes);
                return reply;
            }
        }
        char buffer[4096];
        const int got = recv(socket_, buffer, sizeof(buffer), 0);
        if (got == 0) throw RtspClosed();
        if (got < 0)
            throw std::runtime_error(WSAGetLastError() == WSAETIMEDOUT ? "the receiver does not answer" : "connection lost");
        received.append(buffer, size_t(got));
    }
    throw std::runtime_error("a reply without an end");
}

} // namespace camel::server::airplay
