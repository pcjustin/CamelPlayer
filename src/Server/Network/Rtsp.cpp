// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Rtsp.h"

#include <wil/resource.h>
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
constexpr std::string_view kUserAgent = "AirPlay/999.0.0 CamelPlayer"; // HomePods on OS 27 refuse any other form tried
constexpr std::string_view kClientName = "CamelPlayer";
constexpr std::string_view kEndOfLine = "\r\n";
constexpr std::string_view kTransientHeaders = "X-Apple-HKP: 4\r\n";
constexpr std::string_view kPairingHeaders = "X-Apple-HKP: 3\r\n";
constexpr std::string_view kPairingType = "application/octet-stream";
constexpr std::string_view kPairingUser = "Pair-Setup";
constexpr size_t kSecretBytes = 32;
constexpr size_t kKeyBytes = 32;
constexpr int kStatusOk = 200;
enum PairingTag : uint8_t {
    TagMethod = 0,
    TagIdentifier = 1,
    TagSalt = 2,
    TagPublicKey = 3,
    TagProof = 4,
    TagEncryptedData = 5,
    TagState = 6,
    TagError = 7,
    TagSignature = 10,
    TagFlags = 19
};
constexpr char kErrorAuthentication = 2;

std::string messageNonce(std::string_view name) {
    return std::string(4, '\0') + std::string(name);
}

int numberIn(std::string_view text) {
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

} // namespace

// Six bytes of a hash of the computer's name, marked as a locally administered address.
const std::string& senderDeviceId() {
    static const std::string id = [] {
        char host[256] = {};
        gethostname(host, int(sizeof(host)) - 1);
        const std::string digest = sha512(std::string("CamelPlayer ") + host);
        std::string text = std::format("0x{:02x}", (uint8_t(digest[0]) & 0xFC) | 0x02);
        for (size_t i = 1; i < 6; ++i) text += std::format("{:02x}", uint8_t(digest[i]));
        return text;
    }();
    return id;
}

RtspReply parseRtspHead(std::string_view head) {
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

RtspClient::RtspClient(const std::string& address, uint16_t port, int timeoutMs) {
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
        closesocket(socket_);
        throw std::runtime_error("not an address");
    }
    if (connectWithin(socket_, reinterpret_cast<const sockaddr*>(&target), sizeof(target),
                      std::chrono::milliseconds(timeoutMs)) != Reach::Connected) {
        closesocket(socket_);
        throw std::runtime_error("the receiver does not accept a connection");
    }
    u_long blocking = 0;
    ioctlsocket(socket_, FIONBIO, &blocking);
    setTimeout(timeoutMs);

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
}

std::string RtspClient::url() const {
    return std::format("rtsp://{}/{}", localAddress_, session_);
}

void RtspClient::setTimeout(int timeoutMs) {
    timeoutMs_ = timeoutMs;
    const auto timeout = DWORD(timeoutMs);
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
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
    // A Mac names the sender from X-Apple-Client-Name when it asks its user to accept it.
    std::string message = std::format("{} {} RTSP/1.0\r\nCSeq: {}\r\nUser-Agent: {}\r\nClient-Instance: "
                                      "{}\r\nX-Apple-Client-Name: {}\r\nX-Apple-Device-ID: {}\r\n",
                                      method, url, ++sequence_, kUserAgent, instance_, kClientName, senderDeviceId());
    if (!sessionHeader_.empty()) message += std::format("Session: {}\r\n", sessionHeader_);
    message += headers;
    if (!body.empty()) message += std::format("Content-Type: {}\r\nContent-Length: {}\r\n", contentType, body.size());
    message += kEndOfLine;
    message += body;
    if (channel_) message = channel_->seal(message);
    if (send(socket_, message.data(), int(message.size()), 0) != int(message.size())) throw RtspClosed();

    std::string received, sealed;
    while (received.size() <= kMaxReplyBytes) {
        const size_t headEnd = received.find(kEndOfHeaders);
        if (headEnd != std::string::npos) {
            RtspReply reply = parseRtspHead(std::string_view(received).substr(0, headEnd));
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
        if (!channel_) {
            received.append(buffer, size_t(got));
            continue;
        }
        sealed.append(buffer, size_t(got));
        const auto opened = channel_->open(sealed);
        if (!opened) throw std::runtime_error("a damaged reply");
        received += *opened;
    }
    throw std::runtime_error("a reply without an end");
}

std::map<uint8_t, std::string> RtspClient::pairingStep(std::string_view path, std::string_view headers, const TlvItems& items,
                                                       bool& answered) {
    const auto reply = request("POST", path, kPairingType, tlv(items), headers);
    answered = reply.status == kStatusOk;
    return parseTlv(reply.body);
}

std::optional<SrpProof> RtspClient::exchangeProofs(std::string_view pin, bool transient, PairOutcome& failure) {
    failure = PairOutcome::Refused;
    const auto headers = transient ? kTransientHeaders : kPairingHeaders;
    TlvItems first{{TagMethod, std::string(1, '\0')}, {TagState, "\x01"}};
    if (transient) first.emplace_back(TagFlags, "\x10");
    bool answered = false;
    auto items = pairingStep("/pair-setup", headers, first, answered);
    if (!answered || items.contains(TagError) || !items.contains(TagSalt) || !items.contains(TagPublicKey)) return std::nullopt;
    auto proof = srpClient(kPairingUser, pin, items[TagSalt], items[TagPublicKey], randomBytes(kSecretBytes));
    if (proof.proof.empty()) return std::nullopt;
    items = pairingStep("/pair-setup", headers, {{TagState, "\x03"}, {TagPublicKey, proof.publicKey}, {TagProof, proof.proof}},
                        answered);
    const auto error = items.find(TagError);
    if (answered && error != items.end() && error->second == std::string(1, kErrorAuthentication))
        failure = PairOutcome::WrongCode;
    if (!answered || error != items.end() || items[TagProof] != proof.expected) return std::nullopt;
    return proof;
}

PairOutcome RtspClient::pairTransiently(std::string_view pin) {
    PairOutcome failure = PairOutcome::Refused;
    const auto proof = exchangeProofs(pin, true, failure);
    if (!proof) return failure;
    channel_ = SecureChannel::forSender(proof->sessionKey);
    sessionKey_ = proof->sessionKey;
    return PairOutcome::Paired;
}

bool RtspClient::showPin(int userTimeoutMs) {
    const int usual = timeoutMs_;
    setTimeout(userTimeoutMs);
    const auto restore = wil::scope_exit([&] { setTimeout(usual); });
    return request("POST", "/pair-pin-start", kPairingType, {}, kPairingHeaders).status == kStatusOk;
}

PairOutcome RtspClient::pairSetup(std::string_view pin, const PairingIdentity& me, PairedReceiver& receiver) {
    PairOutcome failure = PairOutcome::Refused;
    const auto proof = exchangeProofs(pin, false, failure);
    if (!proof) return failure;
    const std::string& shared = proof->sessionKey;
    const std::string key = hkdfSha512(shared, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", kKeyBytes);
    const std::string publicKey = ed25519PublicKey(me.seed);
    const std::string signedInfo =
        hkdfSha512(shared, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", kKeyBytes) + me.id + publicKey;
    const std::string mine =
        tlv({{TagIdentifier, me.id}, {TagPublicKey, publicKey}, {TagSignature, ed25519Sign(me.seed, signedInfo)}});
    bool answered = false;
    auto items =
        pairingStep("/pair-setup", kPairingHeaders,
                    {{TagState, "\x05"}, {TagEncryptedData, airplay::seal(key, messageNonce("PS-Msg05"), {}, mine)}}, answered);
    if (!answered || items.contains(TagError)) return PairOutcome::Refused;
    const auto opened = airplay::open(key, messageNonce("PS-Msg06"), {}, items[TagEncryptedData]);
    if (!opened) return PairOutcome::Refused;
    auto theirs = parseTlv(*opened);
    const std::string accessory =
        hkdfSha512(shared, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", kKeyBytes) +
        theirs[TagIdentifier] + theirs[TagPublicKey];
    if (theirs[TagIdentifier].empty() || !ed25519Verify(theirs[TagPublicKey], accessory, theirs[TagSignature]))
        return PairOutcome::Refused;
    receiver = {theirs[TagIdentifier], theirs[TagPublicKey]};
    return PairOutcome::Paired;
}

bool RtspClient::pairVerify(const PairingIdentity& me, const PairedReceiver& receiver) {
    const std::string secret = randomBytes(kKeyBytes);
    const std::string mine = x25519Base(secret);
    bool answered = false;
    auto items = pairingStep("/pair-verify", kPairingHeaders, {{TagState, "\x01"}, {TagPublicKey, mine}}, answered);
    if (!answered || items.contains(TagError) || items[TagPublicKey].size() != kKeyBytes) return false;
    const std::string theirs = items[TagPublicKey];
    const std::string shared = x25519(secret, theirs);
    const std::string key = hkdfSha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", kKeyBytes);
    const auto opened = airplay::open(key, messageNonce("PV-Msg02"), {}, items[TagEncryptedData]);
    if (!opened) return false;
    auto proof = parseTlv(*opened);
    if (proof[TagIdentifier] != receiver.id ||
        !ed25519Verify(receiver.publicKey, theirs + receiver.id + mine, proof[TagSignature]))
        return false;
    const std::string answer = tlv({{TagIdentifier, me.id}, {TagSignature, ed25519Sign(me.seed, mine + me.id + theirs)}});
    items =
        pairingStep("/pair-verify", kPairingHeaders,
                    {{TagState, "\x03"}, {TagEncryptedData, airplay::seal(key, messageNonce("PV-Msg03"), {}, answer)}}, answered);
    if (!answered || items.contains(TagError)) return false;
    channel_ = SecureChannel::forSender(shared);
    sessionKey_ = shared;
    return true;
}

} // namespace camel::server::airplay
