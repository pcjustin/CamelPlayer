// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// HTTP GET over WinHTTP for the update check and the online lookups (docs/Plan.md §7.4, §8.6).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "Server/Artwork/ArtworkProvider.h"

namespace camel::server::http {

// "CamelPlayer/<version> (<project URL>)": MusicBrainz and Wikimedia block clients that do not say who they are.
std::string userAgent();

// Streams the body of a GET to `sink` and returns the HTTP status. `onStart` receives the status and Content-Length
// (0 when the server sends none) before the body, and may throw to stop there. Throws std::runtime_error on network
// errors and cancellation.
unsigned get(const std::string& url, const char* accept, const CancellationToken& token,
             const std::function<void(unsigned status, uint64_t length)>& onStart,
             const std::function<void(const char*, size_t)>& sink);

struct Response {
    unsigned status = 0;
    std::string body;
};
// The whole body; throws std::runtime_error past `maxBytes`, on network errors and on cancellation.
Response get(const std::string& url, const char* accept, const CancellationToken& token, size_t maxBytes);

// Percent-encodes UTF-8 text for a query value or a path segment.
std::string encode(std::string_view text);

} // namespace camel::server::http
