// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The Server API's calls (docs/Server-API.md): each IServer call by name with its arguments as a JSON array, its
// result as JSON, and the errors either side throws again as the same exceptions.
#pragma once

#include <nlohmann/json.hpp>

#include <exception>
#include <map>
#include <string>

#include "Server/Api/ServerApi.h"

namespace camel::server::api {

// Runs `call` on `server`. Throws std::invalid_argument for a call it does not know or arguments that do not fit it,
// and whatever the call throws.
nlohmann::json answer(IServer& server, const std::string& call, const nlohmann::json& args, const CancellationToken& token);

// An exception as the error a response carries, and back.
nlohmann::json errorOf(const std::exception_ptr& error);
[[noreturn]] void rethrow(const nlohmann::json& error);

// The queues of a snapshot's JSON as what changed since the last one sent on the same connection, and back.
class QueueEncoder {
public:
    void encode(nlohmann::json& snapshot);

private:
    std::map<std::string, nlohmann::json> sent_; // each zone's entries as last sent; "" for the snapshot's own queue
};
class QueueDecoder {
public:
    // Throws std::invalid_argument for changes that do not fit what it holds.
    void decode(nlohmann::json& snapshot);

private:
    std::map<std::string, nlohmann::json> held_;
};

} // namespace camel::server::api
