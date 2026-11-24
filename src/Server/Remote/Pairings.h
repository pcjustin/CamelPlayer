// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Which Control points may use the Server API (docs/Plan.md §17.8, pairing): an ownerless Server is claimed by the first
// one that asks, without a code; each later one gives a code a paired device was shown, unless it is a computer this one
// paired with (trusted below). Each paired device has a token of its own, kept by the Server only as its SHA-256.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace camel::server {

struct PairedDevice {
    std::string id, name;
    std::string server; // the device's own Server ID, when it told it
    int64_t paired = 0; // ms since 1970
    bool operator==(const PairedDevice&) const = default;
};

class Pairings {
public:
    static constexpr int64_t kCodeLifetimeMs = int64_t{5} * 60 * 1000;
    static constexpr int kWrongCodes = 5;                            // in a row, which stop pairing ...
    static constexpr int64_t kStoppedForMs = int64_t{5} * 60 * 1000; // ... for this long

    // Kept in `file`; an empty path keeps them in memory.
    explicit Pairings(std::filesystem::path file);

    struct Answer {
        std::string token, device; // when it paired
        std::string error;         // "codeNeeded", "wrongCode", "stopped" or "nameTaken"
        std::string suggestion;    // a free name, with "nameTaken"
        int64_t stoppedUntil = 0;  // with "stopped" and with the wrong code that stopped it
        bool stoppedNow = false;   // this wrong code stopped pairing: the paired devices are told
    };
    // `server` is the asking computer's own Server ID; a `trusted` one, a computer this one paired with, needs no code.
    Answer pair(const std::string& name, const std::string& code, int64_t now, const std::string& server = {},
                bool trusted = false);
    // A code for the next device, six digits, valid for one pairing within kCodeLifetimeMs; a new one replaces it.
    std::string newCode(int64_t now);
    // The device a token belongs to; nothing for one never given or revoked.
    std::optional<PairedDevice> deviceOf(const std::string& token) const;
    std::vector<PairedDevice> devices() const;
    bool revoke(const std::string& device);
    // A paired device's new name; "nameTaken" with a free one in `suggestion` when another device has it.
    Answer rename(const std::string& device, const std::string& name);
    void resetOwner(); // every device forgotten: the next to ask claims the Server
    bool owned() const;
    // What proves to the computer whose Server ID is `server`, and which paired here, that this one gave it its token:
    // the token's SHA-256, which that computer can work out from the token it keeps.
    std::optional<std::string> proofFor(const std::string& server) const;
    static std::string proofOf(const std::string& token);
    static std::filesystem::path fileIn(const std::filesystem::path& data);

private:
    struct Stored {
        PairedDevice device;
        std::string tokenHash;
    };
    void save() const;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<Stored> stored_;
    bool damaged_ = false;                                // a file it could not read: owned, so pairing needs a code
    std::optional<std::pair<std::string, int64_t>> code_; // the code and when it expires
    int wrong_ = 0;
    int64_t stoppedUntil_ = 0;
};

} // namespace camel::server
