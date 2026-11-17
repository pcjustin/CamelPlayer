// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlayPairings.h"

#include <windows.h>
#include <dpapi.h>

#include <format>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "Common/util/Log.h"

#pragma comment(lib, "crypt32.lib")

namespace camel::server {

namespace {

constexpr size_t kSeedBytes = 32;

std::string hexOf(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) out += std::format("{:02x}", uint8_t(c));
    return out;
}

std::string bytesOf(std::string_view hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(char(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

std::string newPairingId() {
    const std::string r = airplay::randomBytes(16);
    const std::string h = hexOf(r);
    return std::format("{}-{}-{}-{}-{}", h.substr(0, 8), h.substr(8, 4), h.substr(12, 4), h.substr(16, 4), h.substr(20));
}

std::string protect(const std::string& plain, bool unprotect) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    const BOOL done = unprotect ? CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                                : CryptProtectData(&in, L"CamelPlayer AirPlay pairings", nullptr, nullptr, nullptr,
                                                   CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!done) throw std::runtime_error(std::format("DPAPI failed: {}", GetLastError()));
    std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

} // namespace

AirPlayPairings::AirPlayPairings(std::filesystem::path file) : file_(std::move(file)) {
    load();
}

void AirPlayPairings::load() {
    if (file_.empty()) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    try {
        const std::string sealed{std::istreambuf_iterator<char>(in), {}};
        const auto j = nlohmann::json::parse(protect(sealed, true));
        identity_ = {j.at("id").get<std::string>(), bytesOf(j.at("seed").get<std::string>())};
        for (const auto& [id, entry] : j.at("receivers").items()) {
            AirPlayCredentials credentials;
            credentials.password = entry.value("password", "");
            if (entry.contains("pairingId"))
                credentials.keys = airplay::PairedReceiver{entry.at("pairingId").get<std::string>(),
                                                           bytesOf(entry.at("publicKey").get<std::string>())};
            receivers_[id] = std::move(credentials);
        }
    } catch (const std::exception& e) {
        // Paired receivers then ask for their password or code again.
        log::warn("AirPlay pairings could not be read: %s", e.what());
        identity_ = {};
        receivers_.clear();
    }
}

void AirPlayPairings::store() const {
    if (file_.empty()) return;
    nlohmann::json j;
    j["id"] = identity_.id;
    j["seed"] = hexOf(identity_.seed);
    j["receivers"] = nlohmann::json::object();
    for (const auto& [id, credentials] : receivers_) {
        auto& entry = j["receivers"][id];
        if (!credentials.password.empty()) entry["password"] = credentials.password;
        if (credentials.keys) {
            entry["pairingId"] = credentials.keys->id;
            entry["publicKey"] = hexOf(credentials.keys->publicKey);
        }
    }
    const std::string sealed = protect(j.dump(), false);
    auto written = file_;
    written += L".new";
    {
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        out.write(sealed.data(), std::streamsize(sealed.size()));
        out.flush();
        if (!out) throw std::runtime_error("the AirPlay pairings could not be written");
    }
    if (!MoveFileExW(written.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("the AirPlay pairings could not be written");
}

airplay::PairingIdentity AirPlayPairings::identity() {
    std::scoped_lock lock(mutex_);
    if (identity_.id.empty()) {
        identity_ = {newPairingId(), airplay::randomBytes(kSeedBytes)};
        store();
    }
    return identity_;
}

std::optional<AirPlayCredentials> AirPlayPairings::find(const std::string& receiver) const {
    std::scoped_lock lock(mutex_);
    const auto found = receivers_.find(receiver);
    if (found == receivers_.end()) return std::nullopt;
    return found->second;
}

void AirPlayPairings::save(const std::string& receiver, AirPlayCredentials credentials) {
    std::scoped_lock lock(mutex_);
    receivers_[receiver] = std::move(credentials);
    store();
}

void AirPlayPairings::forget(const std::string& receiver) {
    std::scoped_lock lock(mutex_);
    if (receivers_.erase(receiver)) store();
}

bool pairFor(airplay::RtspClient& client, const airplay::Receiver& receiver, AirPlayPairings* pairings) {
    if (const auto credentials = pairings ? pairings->find(receiver.id) : std::nullopt) {
        if (credentials->keys) return client.pairVerify(pairings->identity(), *credentials->keys);
        return client.pairTransiently(credentials->password) == airplay::PairOutcome::Paired;
    }
    if (receiver.transient) return client.pairTransiently() == airplay::PairOutcome::Paired;
    return true;
}

} // namespace camel::server
