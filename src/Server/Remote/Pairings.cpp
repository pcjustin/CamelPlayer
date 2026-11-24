// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/Pairings.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "Common/util/Log.h"

namespace camel::server {
namespace {

using nlohmann::json;

std::string hex(const uint8_t* bytes, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += kDigits[bytes[i] >> 4];
        out += kDigits[bytes[i] & 15];
    }
    return out;
}

template <size_t N> std::array<uint8_t, N> randomBytes() {
    std::array<uint8_t, N> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), ULONG(N), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("pairing: no random bytes");
    return bytes;
}

std::string sha256(const std::string& text) {
    std::array<uint8_t, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                                   ULONG(text.size()), digest.data(), ULONG(digest.size()))))
        throw std::runtime_error("pairing: SHA-256 failed");
    return hex(digest.data(), digest.size());
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

} // namespace

Pairings::Pairings(std::filesystem::path file) : file_(std::move(file)) {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) return;
    try {
        std::ifstream in(file_, std::ios::binary);
        const auto saved = json::parse(in);
        for (const auto& d : saved.at("devices"))
            stored_.push_back({{d.at("id").get<std::string>(), d.at("name").get<std::string>(), d.at("paired").get<int64_t>()},
                               d.at("token").get<std::string>()});
    } catch (const std::exception& e) {
        // A file it cannot read leaves the Server owned, pairing by a code its own window shows, never open to a claim.
        log::warn("pairing: %s cannot be read: %s", file_.filename().string().c_str(), e.what());
        stored_.clear();
        damaged_ = true;
    }
}

void Pairings::save() const {
    if (file_.empty()) return;
    json devices = json::array();
    for (const auto& s : stored_)
        devices.push_back({{"id", s.device.id}, {"name", s.device.name}, {"paired", s.device.paired}, {"token", s.tokenHash}});
    const auto partial = std::filesystem::path(file_).concat(L".tmp");
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << json{{"devices", devices}}.dump(2);
        if (!out) throw std::runtime_error("pairing: cannot write the paired devices");
    }
    std::filesystem::rename(partial, file_);
}

bool Pairings::owned() const {
    std::lock_guard lock(mutex_);
    return damaged_ || !stored_.empty();
}

Pairings::Answer Pairings::rename(const std::string& device, const std::string& name) {
    std::lock_guard lock(mutex_);
    Answer answer;
    const auto taken = [&](const std::string& n) {
        return std::ranges::any_of(stored_,
                                   [&](const Stored& s) { return s.device.id != device && lower(s.device.name) == lower(n); });
    };
    const auto own = std::ranges::find(stored_, device, [](const Stored& s) { return s.device.id; });
    if (own == stored_.end()) {
        answer.error = "notPaired";
        return answer;
    }
    if (name.empty() || taken(name)) {
        answer.error = "nameTaken";
        const std::string base = name.empty() ? own->device.name : name;
        answer.suggestion = base;
        for (int n = 2; taken(answer.suggestion); ++n) answer.suggestion = base + " (" + std::to_string(n) + ")";
        return answer;
    }
    own->device.name = name;
    answer.device = device;
    save();
    return answer;
}

Pairings::Answer Pairings::pair(const std::string& name, const std::string& code, int64_t now) {
    std::lock_guard lock(mutex_);
    Answer answer;
    if (now < stoppedUntil_) {
        answer.error = "stopped";
        answer.stoppedUntil = stoppedUntil_;
        return answer;
    }
    const auto taken = [this](const std::string& n) {
        return std::ranges::any_of(stored_, [&](const Stored& s) { return lower(s.device.name) == lower(n); });
    };
    if (name.empty() || taken(name)) {
        answer.error = "nameTaken";
        const std::string base = name.empty() ? std::string("Control point") : name;
        answer.suggestion = base;
        for (int n = 2; taken(answer.suggestion); ++n) answer.suggestion = base + " (" + std::to_string(n) + ")";
        return answer;
    }
    if (damaged_ || !stored_.empty()) {
        if (code.empty()) {
            answer.error = "codeNeeded";
            return answer;
        }
        if (!code_ || code != code_->first || now > code_->second) {
            answer.error = "wrongCode";
            if (++wrong_ >= kWrongCodes) {
                wrong_ = 0;
                code_.reset();
                stoppedUntil_ = now + kStoppedForMs;
                answer.stoppedUntil = stoppedUntil_;
                answer.stoppedNow = true;
            }
            return answer;
        }
        code_.reset();
    }
    wrong_ = 0;
    const auto id = randomBytes<8>();
    const auto token = randomBytes<32>();
    answer.device = hex(id.data(), id.size());
    answer.token = hex(token.data(), token.size());
    stored_.push_back({{answer.device, name, now}, sha256(answer.token)});
    damaged_ = false;
    save();
    return answer;
}

std::string Pairings::newCode(int64_t now) {
    const auto bytes = randomBytes<4>();
    const uint32_t value = (uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]) % 1'000'000;
    char text[8] = {};
    std::snprintf(text, sizeof(text), "%06u", value);
    std::lock_guard lock(mutex_);
    code_ = {text, now + kCodeLifetimeMs};
    return text;
}

std::optional<PairedDevice> Pairings::deviceOf(const std::string& token) const {
    if (token.empty()) return std::nullopt;
    const auto hash = sha256(token);
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(stored_, hash, &Stored::tokenHash);
    if (found == stored_.end()) return std::nullopt;
    return found->device;
}

std::vector<PairedDevice> Pairings::devices() const {
    std::lock_guard lock(mutex_);
    std::vector<PairedDevice> out;
    out.reserve(stored_.size());
    for (const auto& s : stored_) out.push_back(s.device);
    return out;
}

bool Pairings::revoke(const std::string& device) {
    std::lock_guard lock(mutex_);
    const auto erased = std::erase_if(stored_, [&](const Stored& s) { return s.device.id == device; });
    if (erased) save();
    return erased > 0;
}

void Pairings::resetOwner() {
    std::lock_guard lock(mutex_);
    stored_.clear();
    damaged_ = false;
    code_.reset();
    save();
}

} // namespace camel::server
