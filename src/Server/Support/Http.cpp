// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/Http.h"

#include <windows.h>
#include <winhttp.h>

#include <stdexcept>
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

} // namespace

std::string userAgent() {
    return "CamelPlayer/" CAMEL_VERSION_STRING " (https://github.com/pcjustin/CamelPlayer)";
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
    const Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    const std::wstring headers = L"Accept: " + toWide(accept);
    if (!WinHttpSendRequest(request.get(), headers.c_str(), DWORD(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        fail("HTTP request");
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
        if (!WinHttpReadData(request.get(), buffer.data(), DWORD(buffer.size()), &read)) fail("HTTP read");
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
