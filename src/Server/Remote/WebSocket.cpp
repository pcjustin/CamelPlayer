// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/WebSocket.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace camel::server::websocket {
namespace {

[[noreturn]] void invalid(const std::string& what) {
    throw std::invalid_argument("WebSocket: " + what);
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string base64(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
        out += kDigits[(n >> 18) & 63];
        out += kDigits[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kDigits[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kDigits[n & 63] : '=';
    }
    return out;
}

std::string acceptKey(std::string_view key) {
    const std::string text = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<uint8_t, 20> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                                   ULONG(text.size()), digest.data(), ULONG(digest.size()))))
        throw std::runtime_error("WebSocket: SHA-1 failed");
    return base64(digest);
}

std::string HttpHead::header(const std::string& name) const {
    const auto found = headers.find(name);
    return found == headers.end() ? std::string() : found->second;
}

HttpHead parseHead(std::string_view head) {
    HttpHead out;
    bool first = true;
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto line = head.substr(0, end);
        head = end == std::string_view::npos ? std::string_view() : head.substr(end + 2);
        if (first) {
            first = false;
            const auto a = line.find(' ');
            const auto b = a == std::string_view::npos ? a : line.find(' ', a + 1);
            if (b == std::string_view::npos || a == 0 || b == a + 1) invalid("not an HTTP head");
            out.first = line.substr(0, a);
            out.second = line.substr(a + 1, b - a - 1);
            out.third = line.substr(b + 1);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) invalid("a header without a name");
        out.headers[lower(std::string(trim(line.substr(0, colon))))] = trim(line.substr(colon + 1));
    }
    if (first) invalid("an empty head");
    return out;
}

std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        const int high = i + 2 < text.size() ? hexValue(text[i + 1]) : -1;
        const int low = high < 0 ? -1 : hexValue(text[i + 2]);
        if (low < 0) invalid("a broken percent escape");
        out += char(high << 4 | low);
        i += 2;
    }
    return out;
}

std::string percentEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::map<std::string, std::string> queryOf(std::string_view target) {
    std::map<std::string, std::string> out;
    const auto mark = target.find('?');
    if (mark == std::string_view::npos) return out;
    auto query = target.substr(mark + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        if (part.empty()) continue;
        const auto equals = part.find('=');
        out[percentDecode(part.substr(0, equals))] =
            equals == std::string_view::npos ? std::string() : percentDecode(part.substr(equals + 1));
    }
    return out;
}

std::optional<std::pair<Frame, size_t>> decodeFrame(std::string_view data, bool masked, size_t limit) {
    if (data.size() < 2) return std::nullopt;
    const auto b0 = uint8_t(data[0]), b1 = uint8_t(data[1]);
    if (b0 & 0x70) invalid("reserved bits set");
    const auto opcode = Opcode(b0 & 0x0F);
    const bool control = uint8_t(opcode) >= 8;
    if (opcode != Opcode::Continuation && opcode != Opcode::Text && opcode != Opcode::Binary && opcode != Opcode::Close &&
        opcode != Opcode::Ping && opcode != Opcode::Pong)
        invalid("an unknown opcode");
    if (bool(b1 & 0x80) != masked) invalid(masked ? "an unmasked frame from a client" : "a masked frame from a server");
    uint64_t length = b1 & 0x7F;
    size_t at = 2;
    if (length >= 126) {
        const size_t bytes = length == 126 ? 2 : 8;
        if (data.size() < at + bytes) return std::nullopt;
        length = 0;
        for (size_t i = 0; i < bytes; ++i) length = length << 8 | uint8_t(data[at + i]);
        if (length >> 63) invalid("a length with its top bit set");
        at += bytes;
    }
    if (control && (!(b0 & 0x80) || length > 125)) invalid("a control frame that is fragmented or long");
    if (length > limit) invalid("a frame longer than " + std::to_string(limit) + " bytes");
    std::array<uint8_t, 4> mask{};
    if (masked) {
        if (data.size() < at + 4) return std::nullopt;
        std::copy_n(data.begin() + std::ptrdiff_t(at), 4, mask.begin());
        at += 4;
    }
    if (data.size() - at < length) return std::nullopt;
    Frame frame{bool(b0 & 0x80), opcode, std::string(data.substr(at, size_t(length)))};
    if (masked)
        for (size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] = char(frame.payload[i] ^ mask[i % 4]);
    return std::pair{std::move(frame), at + size_t(length)};
}

std::string encodeFrame(Opcode opcode, std::string_view payload, const std::optional<std::array<uint8_t, 4>>& mask) {
    std::string out;
    out += char(0x80 | uint8_t(opcode));
    const uint8_t maskBit = mask ? 0x80 : 0;
    if (payload.size() < 126) {
        out += char(maskBit | payload.size());
    } else if (payload.size() <= 0xFFFF) {
        out += char(maskBit | 126);
        out += char(payload.size() >> 8);
        out += char(payload.size() & 0xFF);
    } else {
        out += char(maskBit | 127);
        for (int shift = 56; shift >= 0; shift -= 8) out += char((uint64_t(payload.size()) >> shift) & 0xFF);
    }
    if (!mask) return out + std::string(payload);
    out.append(reinterpret_cast<const char*>(mask->data()), 4);
    for (size_t i = 0; i < payload.size(); ++i) out += char(payload[i] ^ (*mask)[i % 4]);
    return out;
}

} // namespace camel::server::websocket
