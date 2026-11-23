// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/ServerTokens.h"

#include <windows.h>
#include <dpapi.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

#include "Common/util/Log.h"

namespace camel::server {
namespace {

std::string protect(const std::string& plain, bool unprotect) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    const BOOL done = unprotect ? CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                                : CryptProtectData(&in, L"CamelPlayer Server tokens", nullptr, nullptr, nullptr,
                                                   CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!done) throw std::runtime_error("DPAPI failed: " + std::to_string(GetLastError()));
    std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

} // namespace

ServerTokens::ServerTokens(std::filesystem::path file) : file_(std::move(file)) {
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) return;
    try {
        std::ifstream in(file_, std::ios::binary);
        const std::string sealed{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        tokens_ = nlohmann::json::parse(protect(sealed, true)).get<std::map<std::string, std::string>>();
    } catch (const std::exception& e) {
        log::warn("Server tokens cannot be read: %s", e.what());
        tokens_.clear();
    }
}

void ServerTokens::save() const {
    const auto sealed = protect(nlohmann::json(tokens_).dump(), false);
    const auto partial = std::filesystem::path(file_).concat(L".tmp");
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(sealed.data(), std::streamsize(sealed.size()));
        if (!out) throw std::runtime_error("Server tokens cannot be written");
    }
    std::filesystem::rename(partial, file_);
}

std::optional<std::string> ServerTokens::token(const std::string& server) const {
    std::lock_guard lock(mutex_);
    const auto found = tokens_.find(server);
    if (found == tokens_.end()) return std::nullopt;
    return found->second;
}

void ServerTokens::set(const std::string& server, const std::string& token) {
    std::lock_guard lock(mutex_);
    tokens_[server] = token;
    save();
}

void ServerTokens::forget(const std::string& server) {
    std::lock_guard lock(mutex_);
    if (tokens_.erase(server)) save();
}

} // namespace camel::server
