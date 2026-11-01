// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The playing track's cover for Discord (docs/Plan.md §8.4). Discord fetches images itself from a public URL, so the cover
// is served on 127.0.0.1 and published through a Cloudflare quick tunnel, which needs no account or domain.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace camel::server {

// The quick tunnel's address in cloudflared's output, if `line` holds one.
std::optional<std::string> quickTunnelUrl(std::string_view line);

class CoverTunnel {
public:
    struct Options {
        std::filesystem::path executable; // cloudflared.exe, downloaded here the first time it is needed
        // A pinned release, checked against its SHA-256 before it ever runs.
        std::string downloadUrl =
            "https://github.com/cloudflare/cloudflared/releases/download/2026.10.7/cloudflared-windows-amd64.exe";
        std::string sha256 = "2493dbc168923bb3e59e5e7716f71deced92992eea9acd6e9d74f84ac7751343";
        // The command line running the tunnel to http://127.0.0.1:<port>; tests run a stand-in.
        std::function<std::wstring(const std::filesystem::path& executable, uint16_t port)> command;
        std::chrono::milliseconds idle = std::chrono::minutes(10), restart{3000};
    };
    enum class State : uint8_t { Off, Downloading, Starting, Ready, Failed };
    struct Status {
        State state = State::Off;
        std::string error; // why the last download or start failed
        bool operator==(const Status&) const = default;
    };

    explicit CoverTunnel(Options options);
    ~CoverTunnel();
    CoverTunnel(const CoverTunnel&) = delete;
    CoverTunnel& operator=(const CoverTunnel&) = delete;

    void setEnabled(bool enabled);
    // The tunnel runs while a track plays and stops `idle` after playback stopped or paused.
    void setPlaying(bool playing);
    // Serves `jpeg` at a new random path from now on; empty serves nothing.
    void serve(std::vector<uint8_t> jpeg);
    // The public URL of what serve() last got; empty without a tunnel or a cover.
    std::string url() const;
    Status status() const;

private:
    void run(const std::stop_token& stop);
    void serveHttp(const std::stop_token& stop);
    bool prepare(const std::stop_token& stop);
    bool start();
    void halt(); // ends cloudflared
    void fail(std::string error);

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool enabled_ = false, playing_ = false, verified_ = false;
    std::chrono::steady_clock::time_point idleSince_{}, startAt_{};
    uint64_t version_ = 0;
    Status status_;
    std::string tunnel_; // https://<name>.trycloudflare.com once cloudflared reports it
    std::string key_;    // the served cover's path, without the slash
    std::vector<uint8_t> cover_;
    uint64_t listener_ = ~0ull; // the 127.0.0.1 socket
    uint16_t port_ = 0;
    void* process_ = nullptr; // cloudflared, in a job that ends it with CamelPlayer
    void* job_ = nullptr;
    std::jthread reader_, server_, worker_;
};

} // namespace camel::server
