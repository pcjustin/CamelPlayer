// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A minimal HTTP/1.1 server on 127.0.0.1 that stands in for the services the Server talks to.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace testutil {

class FakeHttp {
public:
    FakeHttp() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        listen(listener_, 8);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeHttp() {
        closesocket(listener_);
        thread_.join();
        WSACleanup();
    }
    FakeHttp(const FakeHttp&) = delete;
    FakeHttp& operator=(const FakeHttp&) = delete;

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    // Answers a request whose target (path and query) is `target`, or, failing an exact route, starts with it; the
    // longest such route wins.
    void route(const std::string& target, std::string body, int status = 200) {
        std::lock_guard lock(mutex_);
        routes_[target] = {status, std::move(body)};
    }
    // Every request's head so far, oldest first.
    std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void serve() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, size_t(read));
            }
            const auto start = request.find(' ') + 1;
            const std::string target = request.substr(start, request.find(' ', start) - start);
            std::pair<int, std::string> response{404, "{}"};
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(request.substr(0, request.find("\r\n\r\n")));
                if (const auto exact = routes_.find(target); exact != routes_.end()) {
                    response = exact->second;
                } else {
                    size_t longest = 0;
                    for (const auto& [prefix, answer] : routes_)
                        if (target.starts_with(prefix) && prefix.size() > longest) {
                            longest = prefix.size();
                            response = answer;
                        }
                }
            }
            const std::string head = "HTTP/1.1 " + std::to_string(response.first) + (response.first == 200 ? " OK" : " Error") +
                                     "\r\nContent-Length: " + std::to_string(response.second.size()) +
                                     "\r\nConnection: close\r\n\r\n";
            const std::string whole = head + response.second;
            for (size_t sent = 0; sent < whole.size();) {
                const int n = send(client, whole.data() + sent, int(std::min<size_t>(whole.size() - sent, 1 << 16)), 0);
                if (n <= 0) break;
                sent += size_t(n);
            }
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<int, std::string>> routes_;
    std::vector<std::string> requests_;
};

} // namespace testutil
