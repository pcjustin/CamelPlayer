// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Integration/CoverTunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <regex>
#include <stdexcept>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"
#include "Server/Support/Updates.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;

// A failed download is tried again this much later, or at once when the setting changes.
constexpr auto kDownloadRetry = std::chrono::minutes(1);

std::string randomKey() {
    uint8_t bytes[16]{};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    for (const auto byte : bytes) {
        key += kHex[byte >> 4];
        key += kHex[byte & 15];
    }
    return key;
}

void sendAll(SOCKET socket, const std::string& data) {
    for (size_t sent = 0; sent < data.size();) {
        const int n = ::send(socket, data.data() + sent, int(data.size() - sent), 0);
        if (n <= 0) return;
        sent += size_t(n);
    }
}

} // namespace

std::optional<std::string> quickTunnelUrl(std::string_view line) {
    static const std::regex pattern(R"(https://[a-z0-9-]+\.trycloudflare\.com)");
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(line.begin(), line.end(), match, pattern) || match.str() == "https://api.trycloudflare.com")
        return std::nullopt;
    return match.str();
}

CoverTunnel::CoverTunnel(Options options) : options_(std::move(options)) {
    if (!options_.command)
        options_.command = [](const std::filesystem::path& executable, uint16_t port) {
            return L"\"" + executable.wstring() + L"\" tunnel --no-autoupdate --url http://127.0.0.1:" + std::to_wstring(port);
        };
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener != INVALID_SOCKET && bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::listen(listener, 8) == 0) {
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        listener_ = uint64_t(listener);
    } else if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    server_ = std::jthread([this](const std::stop_token& stop) { serveHttp(stop); });
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

CoverTunnel::~CoverTunnel() {
    worker_ = {};
    halt();
    server_ = {};
    if (listener_ != ~0ull) closesocket(SOCKET(listener_));
    WSACleanup();
}

void CoverTunnel::setEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    startAt_ = Clock::now();
    if (!enabled) status_ = {};
    ++version_;
    changed_.notify_all();
}

void CoverTunnel::setPlaying(bool playing) {
    std::lock_guard lock(mutex_);
    if (playing_ == playing) return;
    playing_ = playing;
    if (!playing) idleSince_ = Clock::now();
    ++version_;
    changed_.notify_all();
}

void CoverTunnel::serve(std::vector<uint8_t> jpeg) {
    std::lock_guard lock(mutex_);
    cover_ = std::move(jpeg);
    key_ = cover_.empty() ? std::string() : randomKey();
}

std::string CoverTunnel::url() const {
    std::lock_guard lock(mutex_);
    if (tunnel_.empty() || key_.empty()) return {};
    return tunnel_ + "/" + key_ + ".jpg";
}

CoverTunnel::Status CoverTunnel::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void CoverTunnel::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        const auto seen = version_;
        const auto changedSince = [&] { return version_ != seen; };
        const auto now = Clock::now();
        const bool wanted = enabled_ && (playing_ || now - idleSince_ < options_.idle);
        if (!wanted) {
            if (process_) {
                lock.unlock();
                halt();
                lock.lock();
                log::info("Cover tunnel: stopped");
            }
            if (status_.state != State::Failed) status_.state = State::Off;
            if (enabled_ && !playing_ && now - idleSince_ < options_.idle)
                changed_.wait_until(lock, stop, idleSince_ + options_.idle, changedSince);
            else
                changed_.wait(lock, stop, changedSince);
            continue;
        }
        if (!process_) {
            if (now < startAt_) {
                changed_.wait_until(lock, stop, startAt_, changedSince);
                continue;
            }
            if (!verified_) {
                lock.unlock();
                const bool ready = prepare(stop);
                lock.lock();
                if (!ready) {
                    startAt_ = Clock::now() + kDownloadRetry;
                    continue;
                }
                verified_ = true;
            }
            lock.unlock();
            const bool started = start();
            lock.lock();
            if (!started) startAt_ = Clock::now() + options_.restart;
            continue;
        }
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            lock.unlock();
            halt();
            lock.lock();
            log::warn("Cover tunnel: cloudflared ended, starting it again");
            status_.state = State::Starting;
            startAt_ = Clock::now() + options_.restart;
            continue;
        }
        auto wake = now + std::chrono::seconds(1);
        if (!playing_ && idleSince_ + options_.idle < wake) wake = idleSince_ + options_.idle;
        changed_.wait_until(lock, stop, wake, changedSince);
    }
}

// cloudflared at its pinned release: kept when its SHA-256 matches, downloaded and checked otherwise.
bool CoverTunnel::prepare(const std::stop_token& stop) {
    std::error_code ec;
    const auto& executable = options_.executable;
    try {
        if (std::filesystem::exists(executable, ec) && update::sha256File(executable) == options_.sha256) return true;
    } catch (const std::exception&) {
    }
    {
        std::lock_guard lock(mutex_);
        status_ = {State::Downloading, {}};
    }
    log::info("Cover tunnel: downloading cloudflared");
    const auto partial = std::filesystem::path(executable.wstring() + L".download");
    try {
        std::filesystem::create_directories(executable.parent_path());
        CancellationToken cancel;
        const std::stop_callback onStop(stop, [&] { cancel.cancel(); });
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + partial.filename().string());
            http::get(
                options_.downloadUrl, "application/octet-stream", cancel,
                [](unsigned status, uint64_t) {
                    if (status != 200) throw std::runtime_error("HTTP " + std::to_string(status));
                },
                [&](const char* data, size_t size) { out.write(data, std::streamsize(size)); });
            if (!out) throw std::runtime_error("cannot write " + partial.filename().string());
        }
        if (update::sha256File(partial) != options_.sha256) throw std::runtime_error("the SHA-256 does not match");
        std::filesystem::rename(partial, executable);
        return true;
    } catch (const std::exception& e) {
        std::filesystem::remove(partial, ec);
        fail(std::string("cloudflared download: ") + e.what());
        return false;
    }
}

bool CoverTunnel::start() {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        fail("cloudflared: no output pipe");
        return false;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    std::wstring command = options_.command(options_.executable, port_);
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                        nullptr, nullptr, &startup, &process);
    const DWORD error = GetLastError();
    CloseHandle(write);
    if (!created) {
        CloseHandle(read);
        fail("cloudflared did not start (error " + std::to_string(error) + ")");
        return false;
    }
    // The job ends cloudflared with CamelPlayer, even when CamelPlayer itself is ended.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    {
        std::lock_guard lock(mutex_);
        process_ = process.hProcess;
        job_ = job;
        tunnel_.clear();
        status_ = {State::Starting, {}};
    }
    log::info("Cover tunnel: cloudflared started");
    reader_ = std::jthread([this, read] {
        std::string pending;
        char buffer[4096];
        DWORD size = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &size, nullptr) && size > 0) {
            pending.append(buffer, size);
            for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                if (const auto url = quickTunnelUrl(std::string_view(pending).substr(0, end))) {
                    std::lock_guard lock(mutex_);
                    if (tunnel_.empty()) {
                        tunnel_ = *url;
                        status_.state = State::Ready;
                        log::info("Cover tunnel: ready");
                    }
                }
                pending.erase(0, end + 1);
            }
        }
        CloseHandle(read);
    });
    return true;
}

void CoverTunnel::halt() {
    HANDLE process = nullptr, job = nullptr;
    {
        std::lock_guard lock(mutex_);
        process = process_;
        job = job_;
        process_ = nullptr;
        job_ = nullptr;
        tunnel_.clear();
    }
    if (process) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    if (job) CloseHandle(job);
    // Its output pipe closed with the process, which ends the reader.
    reader_ = {};
}

void CoverTunnel::fail(std::string error) {
    log::warn("Cover tunnel: %s", error.c_str());
    std::lock_guard lock(mutex_);
    status_ = {State::Failed, std::move(error)};
}

// One request at a time is plenty: only Discord's image proxy asks, once per cover.
void CoverTunnel::serveHttp(const std::stop_token& stop) {
    const auto listener = SOCKET(listener_);
    if (listener_ == ~0ull) return;
    while (!stop.stop_requested()) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval wait{0, 200000};
        if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        const DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const int n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, size_t(n));
        }
        std::string body;
        {
            std::lock_guard lock(mutex_);
            if (!key_.empty() && request.starts_with("GET /" + key_ + ".jpg ")) body.assign(cover_.begin(), cover_.end());
        }
        sendAll(client, body.empty() ? std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                                     : "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}

} // namespace camel::server
