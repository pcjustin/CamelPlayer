// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Server/Api/ServerApi.h"

namespace camel::server {

// One connection's Server API (docs/Server-API.md): each request it reads answered on a thread of its own, so a slow
// listing holds up no other call, and the Server's events pushed to it from a thread of its own, the latest one when
// several come while it sends, each queue as what changed since the last it sent.
class ApiSession {
public:
    // `send` takes one message at a time from any thread; it may fail once the connection is gone.
    using Send = std::function<void(const std::string&)>;
    // Calls of the connection rather than the Server, such as pairing's: an answer and what to do once it is sent, or
    // nothing for a call that is the Server's.
    struct Answer {
        nlohmann::json result;
        std::function<void()> after;
    };
    using Calls = std::function<std::optional<Answer>(const std::string& call, const nlohmann::json& args)>;
    static constexpr size_t kMaxCalls = 32; // a connection's calls at once; one more is answered "busy"

    ApiSession(IServer& server, Send send, nlohmann::json hello, Calls calls = {});
    ~ApiSession(); // cancels the calls under way and waits for them
    ApiSession(const ApiSession&) = delete;
    ApiSession& operator=(const ApiSession&) = delete;

    // Throws std::invalid_argument for a message that is not one of the API's; the connection is then closed.
    void receive(std::string_view message);
    // From the Server worker; it never waits for the connection.
    void push(const Event& event);
    // A notice for the connection, such as pairing having stopped: {"notice": notice}.
    void notify(const nlohmann::json& notice);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace camel::server
