// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/Http.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Common/Version.h"
#include "Common/util/Text.h"

namespace camel::server::http {

namespace {

class Internet {
public:
    explicit Internet(HINTERNET handle) : handle_(handle) {
        if (!handle_) throw std::runtime_error("network error " + std::to_string(GetLastError()));
    }
    ~Internet() { WinHttpCloseHandle(handle_); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};

void fail(const char* what) {
    throw std::runtime_error(std::string(what) + " failed (" + std::to_string(GetLastError()) + ")");
}

// A request closed as soon as its token is cancelled, which ends a resolve, connect, send or read WinHTTP is waiting in.
class CancellableRequest {
public:
    CancellableRequest(HINTERNET request, const CancellationToken& token) : request_(request) {
        if (!request) fail("HTTP request");
        // The token cannot wake anyone, so it is looked at every 20 ms; the request's end wakes the watch at once.
        watch_ = std::jthread([this, token](const std::stop_token& stop) {
            std::mutex mutex;
            std::condition_variable_any idle;
            std::unique_lock lock(mutex);
            while (!idle.wait_for(lock, stop, std::chrono::milliseconds(20), [&] { return token.cancelled(); }))
                if (stop.stop_requested()) return;
            close();
        });
    }
    ~CancellableRequest() {
        watch_ = {};
        close();
    }
    CancellableRequest(const CancellableRequest&) = delete;
    CancellableRequest& operator=(const CancellableRequest&) = delete;
    HINTERNET get() const { return request_; }

private:
    void close() {
        if (const HINTERNET open = open_.exchange(nullptr)) WinHttpCloseHandle(open);
    }

    HINTERNET request_;
    std::atomic<HINTERNET> open_{request_};
    std::jthread watch_;
};

} // namespace

std::string userAgent() {
    return "CamelPlayer/" CAMEL_VERSION_STRING " (https://github.com/pcjustin/CamelPlayer)";
}

std::optional<UrlParts> crackUrl(const std::string& url) {
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUserNameLength = DWORD(-1);
    parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &parts)) return std::nullopt;
    return UrlParts{parts.nScheme == INTERNET_SCHEME_HTTP, parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0,
                    toUtf8(std::wstring_view(parts.lpszHostName, parts.dwHostNameLength))};
}

unsigned get(const std::string& url, const char* accept, const CancellationToken& token,
             const std::function<void(unsigned status, uint64_t length)>& onStart,
             const std::function<void(const char*, size_t)>& sink) {
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &parts)) throw std::runtime_error("invalid URL: " + url);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
    const Internet session(WinHttpOpen(toWide(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000);
    const Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    const CancellableRequest request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0),
                                     token);
    const std::wstring headers = L"Accept: " + toWide(accept);
    if (!WinHttpSendRequest(request.get(), headers.c_str(), DWORD(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        if (token.cancelled()) throw std::runtime_error("cancelled");
        fail("HTTP request");
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t length[32] = {};
    size = sizeof(length);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size,
                            WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length, nullptr, 10);
    if (onStart) onStart(unsigned(status), total);
    std::vector<char> buffer(size_t{64} * 1024);
    for (;;) {
        if (token.cancelled()) throw std::runtime_error("cancelled");
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), DWORD(buffer.size()), &read)) {
            if (token.cancelled()) throw std::runtime_error("cancelled");
            fail("HTTP read");
        }
        if (read == 0) break;
        sink(buffer.data(), read);
    }
    return unsigned(status);
}

Response get(const std::string& url, const char* accept, const CancellationToken& token, size_t maxBytes) {
    Response response;
    response.status = get(url, accept, token, nullptr, [&](const char* data, size_t size) {
        if (response.body.size() + size > maxBytes) throw std::runtime_error("HTTP response too large");
        response.body.append(data, size);
    });
    return response;
}

Reply send(const std::string& method, const std::string& url, const std::string& headers, const std::string& body, int timeoutMs,
           size_t maxBytes) {
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &parts)) throw std::runtime_error("invalid URL: " + url);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
    const Internet session(WinHttpOpen(toWide(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    const Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const Internet request(WinHttpOpenRequest(connection.get(), toWide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (secure) {
        // A player's certificate is its own, signed by no authority Windows knows; the address it answers at has passed
        // the local-network check (upnp::isLanUrl), which is what the request relies on.
        DWORD accept = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_SECURITY_FLAGS, &accept, sizeof(accept));
    }
    const std::wstring wideHeaders = toWide(headers);
    if (!WinHttpSendRequest(request.get(), wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str(),
                            DWORD(wideHeaders.size()), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()),
                            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        fail("HTTP request");
    Reply reply;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    reply.status = status;
    size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    std::wstring head(size / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, head.data(), &size,
                            WINHTTP_NO_HEADER_INDEX))
        reply.headers = toUtf8(head.substr(0, size / sizeof(wchar_t)));
    std::vector<char> buffer(size_t{16} * 1024);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), DWORD(buffer.size()), &read)) fail("HTTP read");
        if (read == 0) break;
        if (reply.body.size() + read > maxBytes) throw std::runtime_error("HTTP response too large");
        reply.body.append(buffer.data(), read);
    }
    return reply;
}

std::string encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = uint8_t(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || c == '-' ||
            c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        }
    }
    return out;
}

} // namespace camel::server::http
