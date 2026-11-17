// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Pairing with an AirPlay receiver that takes a session only from a paired sender (docs/Plan.md §5.10): SRP-6a over the
// RFC 5054 3072-bit group with SHA-512, HKDF-SHA512 and ChaCha20-Poly1305 (RFC 8439), written from their public
// descriptions. Byte strings are std::string, big-endian where they hold a number. No Apple key material.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace camel::server::airplay {

std::string randomBytes(size_t size);
std::string sha512(std::string_view data);
std::string hmacSha512(std::string_view key, std::string_view data);
std::string hkdfSha512(std::string_view secret, std::string_view salt, std::string_view info, size_t size);

std::string chacha20(std::string_view key, uint32_t counter, std::string_view nonce, std::string_view data);
std::string poly1305(std::string_view key, std::string_view message);
// The cipher text with its 16-byte tag; `open` gives nothing when the tag does not match.
std::string seal(std::string_view key, std::string_view nonce, std::string_view aad, std::string_view plain);
std::optional<std::string> open(std::string_view key, std::string_view nonce, std::string_view aad, std::string_view sealed);

// HomeKit's type-length-value items; a value longer than 255 bytes takes several items of the same type.
using TlvItems = std::vector<std::pair<uint8_t, std::string>>;
std::string tlv(const TlvItems& items);
std::map<uint8_t, std::string> parseTlv(std::string_view data);

std::string modPow(std::string_view base, std::string_view exponent, std::string_view modulus);
const std::string& srpPrime(); // the RFC 5054 3072-bit group's, generator 5

// The sender's side of SRP-6a. Empty fields when B is no valid public value.
struct SrpProof {
    std::string publicKey; // A
    std::string proof;     // M1
    std::string sessionKey;
    std::string expected; // the receiver's proof, M2
};
SrpProof srpClient(std::string_view user, std::string_view pin, std::string_view salt, std::string_view receiverKey,
                   std::string_view secret);

// The receiver's side, for a receiver written in a test.
struct SrpServer {
    std::string publicKey; // B
    std::string verifier;
};
SrpServer srpServer(std::string_view user, std::string_view pin, std::string_view salt, std::string_view secret);
// The session key and M2 when the sender's proof holds.
std::optional<std::pair<std::string, std::string>> srpServerCheck(const SrpServer& server, std::string_view user,
                                                                  std::string_view salt, std::string_view senderKey,
                                                                  std::string_view proof, std::string_view secret);

// The RTSP connection once paired: 1024-byte blocks, each led by its length, sealed with the block count as nonce.
class SecureChannel {
public:
    SecureChannel(std::string writeKey, std::string readKey) : writeKey_(std::move(writeKey)), readKey_(std::move(readKey)) {}
    // The keys the sender writes and reads with, from the pairing's session key.
    static SecureChannel forSender(std::string_view sessionKey);
    static SecureChannel forReceiver(std::string_view sessionKey);

    std::string seal(std::string_view plain);
    // The blocks complete in `received`, taken from it and opened; nothing when one is damaged.
    std::optional<std::string> open(std::string& received);

private:
    std::string writeKey_, readKey_;
    uint64_t written_ = 0, read_ = 0;
};

} // namespace camel::server::airplay
