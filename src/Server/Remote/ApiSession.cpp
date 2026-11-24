// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/ApiSession.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "Server/Api/ApiCalls.h"
#include "Server/Api/ApiJson.h"

namespace camel::server {

using nlohmann::json;

namespace {

[[noreturn]] void invalid(const std::string& what) {
    throw std::invalid_argument("API message: " + what);
}

std::string text(const json& message) {
    return message.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace

struct ApiSession::State {
    State(IServer& s, Send sending, Calls answers, Caller who)
        : server(s), send(std::move(sending)), own(std::move(answers)), caller(std::move(who)) {}
    IServer& server;
    Send send;
    Calls own; // the connection's own calls
    Caller caller;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<uint64_t, CancellationToken> calls; // under way, by their ids
    size_t running = 0;                          // call threads not yet ended
    std::optional<Event> pending;                // the latest event not yet sent
    bool completed = false;                      // a command completed in the events `pending` replaced
    api::QueueEncoder encoder;                   // the pusher's alone
    std::jthread pusher;

    void sendQuietly(const json& message) {
        try {
            send(text(message));
        } catch (const std::exception&) {
            // The connection is gone; its reader ends the session.
        }
    }
};

ApiSession::ApiSession(IServer& server, Send send, json hello, Calls calls, Caller caller)
    : state_(std::make_shared<State>(server, std::move(send), std::move(calls), std::move(caller))) {
    state_->sendQuietly({{"hello", std::move(hello)}});
    state_->pusher = std::jthread([s = state_.get()](const std::stop_token& stop) {
        for (;;) {
            Event event;
            bool completed = false;
            {
                std::unique_lock lock(s->mutex);
                if (!s->changed.wait(lock, stop, [s] { return s->pending.has_value(); })) return;
                event = std::move(*s->pending);
                s->pending.reset();
                completed = std::exchange(s->completed, false) || event.kind == EventKind::CommandCompleted;
            }
            json snapshot = toJson(event.snapshot);
            s->encoder.encode(snapshot);
            s->sendQuietly({{"event", completed ? "commandCompleted" : "stateChanged"}, {"snapshot", std::move(snapshot)}});
        }
    });
    push({EventKind::StateChanged, server.snapshot()});
}

ApiSession::~ApiSession() {
    state_->pusher = {};
    std::unique_lock lock(state_->mutex);
    for (auto& [id, token] : state_->calls) token.cancel();
    state_->changed.wait(lock, [this] { return state_->running == 0; });
}

void ApiSession::receive(std::string_view message) {
    json j;
    try {
        j = json::parse(message);
    } catch (const json::exception& e) {
        invalid(e.what());
    }
    if (!j.is_object()) invalid("not an object");
    if (j.contains("cancel")) {
        if (!j["cancel"].is_number_unsigned()) invalid("a cancel without a call id");
        std::lock_guard lock(state_->mutex);
        if (const auto call = state_->calls.find(j["cancel"].get<uint64_t>()); call != state_->calls.end()) call->second.cancel();
        return;
    }
    if (!j.contains("id") || !j["id"].is_number_unsigned() || !j.contains("call") || !j["call"].is_string())
        invalid("neither a call nor a cancel");
    const auto id = j["id"].get<uint64_t>();
    auto call = j["call"].get<std::string>();
    json args = j.contains("args") ? j["args"] : json::array();
    CancellationToken token;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->calls.contains(id)) invalid("a call id already under way");
        if (state_->calls.size() >= kMaxCalls) {
            state_->sendQuietly({{"id", id}, {"error", {{"code", "busy"}, {"message", "too many calls at once"}}}});
            return;
        }
        state_->calls.emplace(id, token);
        ++state_->running;
    }
    std::thread([s = state_, id, call = std::move(call), args = std::move(args), token] {
        Caller who;
        {
            std::lock_guard lock(s->mutex);
            who = s->caller;
        }
        const CallerScope as(std::move(who));
        json reply = {{"id", id}};
        std::function<void()> after;
        try {
            std::optional<Answer> own = s->own ? s->own(call, args) : std::nullopt;
            if (own) {
                reply["result"] = std::move(own->result);
                after = std::move(own->after);
            } else {
                reply["result"] = api::answer(s->server, call, args, token);
            }
        } catch (...) {
            reply["error"] = api::errorOf(std::current_exception());
        }
        if (token.cancelled()) reply = {{"id", id}, {"error", {{"code", "cancelled"}, {"message", "cancelled"}}}};
        s->sendQuietly(reply);
        if (after) after();
        std::lock_guard lock(s->mutex);
        s->calls.erase(id);
        --s->running;
        s->changed.notify_all();
    }).detach();
}

void ApiSession::setCallerName(const std::string& name) {
    std::lock_guard lock(state_->mutex);
    state_->caller.name = name;
}

void ApiSession::notify(const json& notice) {
    state_->sendQuietly({{"notice", notice}});
}

void ApiSession::push(const Event& event) {
    std::lock_guard lock(state_->mutex);
    state_->completed = state_->completed || (state_->pending && state_->pending->kind == EventKind::CommandCompleted);
    state_->pending = event;
    state_->changed.notify_all();
}

} // namespace camel::server
