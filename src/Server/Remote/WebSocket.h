// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The Server API's transport on the LAN (docs/Plan.md §17.8): HTTP/1.1 request heads and RFC 6455 WebSocket frames,
// enough for the Server API's own server and client and nothing more.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace camel::server::websocket {

std::string base64(std::span<const uint8_t> bytes);
// The Sec-WebSocket-Accept answering a client's Sec-WebSocket-Key.
std::string acceptKey(std::string_view key);

// A request or response head, without its blank line: the first line's three parts and the headers, their names in
// lower case; a repeated header keeps its last value.
struct HttpHead {
    std::string first, second, third; // method, target, version; or version, status, reason
    std::map<std::string, std::string> headers;
    std::string header(const std::string& name) const;
};
// Throws std::invalid_argument for a head that is not HTTP.
HttpHead parseHead(std::string_view head);
// The parts of a target's query, decoded; a name without a value maps to an empty string.
std::map<std::string, std::string> queryOf(std::string_view target);
std::string percentDecode(std::string_view text);
std::string percentEncode(std::string_view text);

enum class Opcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct Frame {
    bool fin = true;
    Opcode opcode = Opcode::Text;
    std::string payload;
};

// A frame at the start of `data` and the bytes it took, or nothing while more are needed. `masked` is whether the
// peer must mask (a client does, a server does not). Throws std::invalid_argument for a frame that breaks RFC 6455 or
// whose payload is longer than `limit`.
std::optional<std::pair<Frame, size_t>> decodeFrame(std::string_view data, bool masked, size_t limit);
// A whole frame; a client gives the mask it chose.
std::string encodeFrame(Opcode opcode, std::string_view payload, const std::optional<std::array<uint8_t, 4>>& mask = {});

} // namespace camel::server::websocket
