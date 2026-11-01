// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Integration/DiscordRpc.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::server {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Each frame is the opcode and the payload's length, both little-endian, before the UTF-8 JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

bool writeFrame(HANDLE pipe, uint32_t op, const std::string& payload) {
    std::string frame(8 + payload.size(), '\0');
    const auto length = uint32_t(payload.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, payload.data(), payload.size());
    DWORD written = 0;
    return WriteFile(pipe, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
}

// The bytes waiting in the pipe; nullopt once Discord has closed it.
std::optional<DWORD> available(HANDLE pipe) {
    DWORD bytes = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr)) return std::nullopt;
    return bytes;
}

bool readExactly(HANDLE pipe, char* data, DWORD size) {
    while (size > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, data, size, &read, nullptr) || read == 0) return false;
        data += read;
        size -= read;
    }
    return true;
}

struct Message {
    uint32_t op = 0;
    json body;
};

// The next frame within `timeout`; nullopt when none came, the pipe closed or the frame was broken.
std::optional<Message> readFrame(HANDLE pipe, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto bytes = available(pipe);
        if (!bytes) return std::nullopt;
        if (*bytes >= 8) break;
        if (Clock::now() >= deadline) return std::nullopt;
        Sleep(10);
    }
    uint32_t header[2]{};
    if (!readExactly(pipe, reinterpret_cast<char*>(header), sizeof(header)) || header[1] > (1u << 20)) return std::nullopt;
    std::string payload(header[1], '\0');
    if (!readExactly(pipe, payload.data(), header[1])) return std::nullopt;
    return Message{header[0], json::parse(payload, nullptr, false)};
}

std::string field(const json& body, const char* name) {
    if (!body.is_object() || !body.contains(name) || !body[name].is_string()) return {};
    return body[name].get<std::string>();
}

} // namespace

std::string discordText(const std::string& text) {
    std::wstring wide = toWide(text);
    if (wide.empty()) return {};
    if (wide.size() == 1) wide += L' ';
    if (wide.size() > 128) {
        wide.resize(128);
        if (IS_HIGH_SURROGATE(wide.back())) wide.pop_back();
    }
    return toUtf8(wide);
}

std::string discordActivityJson(const DiscordActivity& activity, bool statusDisplay) {
    // Listening draws the progress bar; status_display_type 2 puts the title in the friends list.
    json result = {{"type", 2}, {"instance", false}};
    if (statusDisplay) result["status_display_type"] = 2;
    if (auto title = discordText(activity.title); !title.empty()) result["details"] = title;
    if (auto artists = discordText(activity.artists); !artists.empty()) result["state"] = artists;
    json timestamps = {{"start", activity.startMs}};
    if (activity.endMs > activity.startMs) timestamps["end"] = activity.endMs;
    result["timestamps"] = timestamps;
    json assets = {{"large_image", activity.largeImage.empty() ? "camelplayer" : activity.largeImage}};
    if (auto album = discordText(activity.album); !album.empty()) assets["large_text"] = album;
    if (!activity.smallImage.empty()) {
        assets["small_image"] = activity.smallImage;
        if (auto text = discordText(activity.smallText); !text.empty()) assets["small_text"] = text;
    }
    result["assets"] = assets;
    return result.dump();
}

bool discordActivityChanged(const std::optional<DiscordActivity>& shown, const std::optional<DiscordActivity>& next) {
    if (!shown || !next) return shown.has_value() != next.has_value();
    constexpr int64_t kSeekMs = 5000;
    const auto length = [](const DiscordActivity& a) { return a.endMs ? a.endMs - a.startMs : 0; };
    if (std::llabs(shown->startMs - next->startMs) > kSeekMs || std::llabs(length(*shown) - length(*next)) > 1000) return true;
    DiscordActivity same = *next;
    same.startMs = shown->startMs;
    same.endMs = shown->endMs;
    return same != *shown;
}

DiscordClient::DiscordClient(Options options)
    : options_(std::move(options)), worker_([this](const std::stop_token& stop) { run(stop); }) {}

DiscordClient::~DiscordClient() = default;

void DiscordClient::setEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    connectAt_ = Clock::now();
    ++version_;
    changed_.notify_all();
}

void DiscordClient::show(const std::optional<DiscordActivity>& activity) {
    std::lock_guard lock(mutex_);
    if (!discordActivityChanged(wanted_, activity)) return;
    wanted_ = activity;
    dirty_ = true;
    sendAt_ = Clock::now() + options_.merge;
    ++version_;
    changed_.notify_all();
}

DiscordClient::State DiscordClient::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void DiscordClient::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        const auto seen = version_;
        const auto changedSince = [&] { return version_ != seen; };
        if (!enabled_) {
            if (pipe_) {
                lock.unlock();
                send(std::nullopt);
                close();
                lock.lock();
            }
            state_ = State::Off;
            changed_.wait(lock, stop, changedSince);
            continue;
        }
        if (!pipe_) {
            if (Clock::now() < connectAt_) {
                changed_.wait_until(lock, stop, connectAt_, changedSince);
                continue;
            }
            lock.unlock();
            const bool connected = connect();
            lock.lock();
            state_ = connected ? State::Connected : State::Searching;
            if (!connected) {
                connectAt_ = Clock::now() + options_.reconnect;
                continue;
            }
            // A new connection shows nothing until told again.
            dirty_ = true;
            sendAt_ = Clock::now();
        }
        if (dirty_ && Clock::now() >= sendAt_) {
            const auto activity = wanted_;
            dirty_ = false;
            lock.unlock();
            const bool sent = send(activity);
            lock.lock();
            if (!sent) {
                dirty_ = true;
                if (pipe_) {
                    sendAt_ = Clock::now() + options_.retry;
                } else {
                    state_ = State::Searching;
                    connectAt_ = Clock::now() + options_.reconnect;
                }
            }
            continue;
        }
        // Between updates, pings are answered and Discord quitting is noticed.
        lock.unlock();
        const bool alive = drain();
        lock.lock();
        if (!alive) {
            close();
            dirty_ = true;
            state_ = State::Searching;
            connectAt_ = Clock::now() + options_.reconnect;
            continue;
        }
        const auto wake = dirty_ ? sendAt_ : Clock::now() + std::chrono::seconds(1);
        changed_.wait_until(lock, stop, wake, changedSince);
    }
    if (pipe_) {
        lock.unlock();
        send(std::nullopt);
        close();
    }
}

bool DiscordClient::connect() {
    for (int i = 0; i < 10; ++i) {
        const auto name = options_.pipePrefix + std::to_wstring(i);
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) continue;
        const json handshake = {{"v", 1}, {"client_id", options_.clientId}};
        if (writeFrame(pipe, kHandshake, handshake.dump()))
            if (const auto ready = readFrame(pipe, options_.timeout);
                ready && ready->op == kFrame && field(ready->body, "evt") == "READY") {
                pipe_ = pipe;
                log::info("Discord: connected on pipe %d", i);
                return true;
            }
        CloseHandle(pipe);
    }
    return false;
}

// False when Discord refused the update (to be retried) or the pipe closed (pipe_ is then null).
bool DiscordClient::send(const std::optional<DiscordActivity>& activity) {
    json args = {{"pid", GetCurrentProcessId()}};
    if (activity) args["activity"] = json::parse(discordActivityJson(*activity, statusDisplay_));
    const std::string nonce = std::to_string(++nonce_);
    const json command = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", nonce}};
    if (!writeFrame(pipe_, kFrame, command.dump())) {
        close();
        return false;
    }
    // The reply carries the nonce; a ping may come first. Without a reply in time the update stands.
    const auto deadline = Clock::now() + options_.timeout;
    while (Clock::now() < deadline) {
        const auto reply = readFrame(pipe_, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
        if (!reply) {
            if (available(pipe_)) return true;
            close();
            return false;
        }
        if (reply->op == kPing) {
            writeFrame(pipe_, kPong, reply->body.dump());
            continue;
        }
        if (reply->op == kClose) {
            close();
            return false;
        }
        if (field(reply->body, "nonce") != nonce) continue;
        if (field(reply->body, "evt") != "ERROR") return true;
        const auto& data = reply->body.contains("data") ? reply->body["data"] : json();
        log::warn("Discord refused the activity: %s", field(data, "message").c_str());
        // Older Discord versions refuse the newest field; one retry goes without it, a second refusal is final.
        if (!statusDisplay_) return true;
        statusDisplay_ = false;
        return false;
    }
    return true;
}

bool DiscordClient::drain() {
    for (;;) {
        const auto bytes = available(pipe_);
        if (!bytes) return false;
        if (*bytes < 8) return true;
        const auto message = readFrame(pipe_, options_.timeout);
        if (!message) return available(pipe_).has_value();
        if (message->op == kPing) writeFrame(pipe_, kPong, message->body.dump());
        if (message->op == kClose) return false;
    }
}

void DiscordClient::close() {
    if (!pipe_) return;
    CloseHandle(pipe_);
    pipe_ = nullptr;
}

} // namespace camel::server
