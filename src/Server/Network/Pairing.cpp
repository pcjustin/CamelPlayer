// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Pairing.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace camel::server::airplay {

namespace {

constexpr size_t kBlockBytes = 1024;
constexpr size_t kTagBytes = 16;
constexpr size_t kPrimeBytes = 384;
constexpr uint8_t kGenerator = 5;
constexpr int kLimbBits = 32;

std::string hashWith(BCRYPT_ALG_HANDLE algorithm, std::string_view key, std::string_view data) {
    std::string out(64, '\0');
    // BCryptHash takes no const input, though it only reads it.
    if (BCryptHash(algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), ULONG(key.size()),
                   reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()),
                   reinterpret_cast<PUCHAR>(out.data()), ULONG(out.size())) != 0)
        throw std::runtime_error("SHA-512 is not available");
    return out;
}

// Numbers as 32-bit limbs, least significant first.
using Limbs = std::vector<uint32_t>;

Limbs fromBigEndian(std::string_view bytes) {
    Limbs limbs((bytes.size() + 3) / 4, 0);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const size_t bit = (bytes.size() - 1 - i) * 8;
        limbs[bit / kLimbBits] |= uint32_t(uint8_t(bytes[i])) << (bit % kLimbBits);
    }
    return limbs;
}

std::string toBigEndian(const Limbs& limbs, size_t size = 0) {
    std::string bytes;
    for (size_t i = limbs.size() * 4; i-- > 0;) bytes.push_back(char((limbs[i / 4] >> (i % 4 * 8)) & 0xFF));
    const size_t first = std::min(bytes.find_first_not_of('\0'), bytes.size());
    bytes.erase(0, first);
    if (bytes.size() < size) bytes.insert(0, size - bytes.size(), '\0');
    return bytes;
}

void trim(Limbs& a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}

int compare(Limbs a, Limbs b) {
    trim(a);
    trim(b);
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

Limbs add(const Limbs& a, const Limbs& b) {
    Limbs sum(std::max(a.size(), b.size()) + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < sum.size(); ++i) {
        carry += uint64_t(i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        sum[i] = uint32_t(carry);
        carry >>= kLimbBits;
    }
    trim(sum);
    return sum;
}

// a - b, for a not below b.
Limbs subtract(const Limbs& a, const Limbs& b) {
    Limbs difference(a);
    int64_t borrow = 0;
    for (size_t i = 0; i < difference.size(); ++i) {
        const int64_t value = int64_t(difference[i]) - (i < b.size() ? b[i] : 0) - borrow;
        borrow = value < 0 ? 1 : 0;
        difference[i] = uint32_t(value + (borrow << kLimbBits));
    }
    trim(difference);
    return difference;
}

Limbs multiply(const Limbs& a, const Limbs& b) {
    Limbs product(a.size() + b.size() + 1, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            carry += uint64_t(a[i]) * b[j] + product[i + j];
            product[i + j] = uint32_t(carry);
            carry >>= kLimbBits;
        }
        for (size_t k = i + b.size(); carry; ++k) {
            carry += product[k];
            product[k] = uint32_t(carry);
            carry >>= kLimbBits;
        }
    }
    trim(product);
    return product;
}

Limbs reduce(const Limbs& a, const Limbs& modulus) {
    Limbs rest;
    for (size_t bit = a.size() * kLimbBits; bit-- > 0;) {
        rest = add(rest, rest);
        if ((a[bit / kLimbBits] >> (bit % kLimbBits)) & 1) rest = add(rest, {1});
        if (compare(rest, modulus) >= 0) rest = subtract(rest, modulus);
    }
    return rest;
}

// Multiplication modulo an odd number in Montgomery's form.
class Montgomery {
public:
    explicit Montgomery(Limbs modulus) : n_(std::move(modulus)) {
        trim(n_);
        uint32_t inverse = 1;
        for (int i = 0; i < 5; ++i) inverse *= 2 - n_[0] * inverse;
        n0_ = uint32_t(0) - inverse;
        Limbs r2{1};
        for (size_t i = 0; i < 2 * n_.size() * kLimbBits; ++i) {
            r2 = add(r2, r2);
            if (compare(r2, n_) >= 0) r2 = subtract(r2, n_);
        }
        r2_ = r2;
    }

    const Limbs& modulus() const { return n_; }

    Limbs product(const Limbs& a, const Limbs& b) const { return montgomery(montgomery(a, b), r2_); }

    Limbs power(const Limbs& base, std::string_view exponent) const {
        const Limbs b = montgomery(reduce(base, n_), r2_);
        Limbs result = montgomery({1}, r2_);
        for (const char byte : exponent)
            for (int bit = 7; bit >= 0; --bit) {
                result = montgomery(result, result);
                if ((uint8_t(byte) >> bit) & 1) result = montgomery(result, b);
            }
        return montgomery(result, {1});
    }

private:
    Limbs montgomery(Limbs a, Limbs b) const {
        const size_t s = n_.size();
        a.resize(s, 0);
        b.resize(s, 0);
        Limbs t(s + 2, 0);
        for (size_t i = 0; i < s; ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < s; ++j) {
                carry += uint64_t(a[j]) * b[i] + t[j];
                t[j] = uint32_t(carry);
                carry >>= kLimbBits;
            }
            carry += t[s];
            t[s] = uint32_t(carry);
            t[s + 1] = uint32_t(carry >> kLimbBits);
            const uint32_t m = t[0] * n0_;
            carry = (uint64_t(m) * n_[0] + t[0]) >> kLimbBits;
            for (size_t j = 1; j < s; ++j) {
                carry += uint64_t(m) * n_[j] + t[j];
                t[j - 1] = uint32_t(carry);
                carry >>= kLimbBits;
            }
            carry += t[s];
            t[s - 1] = uint32_t(carry);
            t[s] = t[s + 1] + uint32_t(carry >> kLimbBits);
            t[s + 1] = 0;
        }
        trim(t);
        if (compare(t, n_) >= 0) t = subtract(t, n_);
        return t;
    }

    Limbs n_, r2_;
    uint32_t n0_ = 0;
};

const Montgomery& srpGroup() {
    static const Montgomery group(fromBigEndian(srpPrime()));
    return group;
}

std::string padded(std::string_view number) {
    return std::string(kPrimeBytes - std::min(number.size(), kPrimeBytes), '\0') + std::string(number);
}

Limbs hashNumber(std::string_view data) {
    return fromBigEndian(sha512(data));
}

std::string multiplier() {
    return sha512(srpPrime() + padded(std::string(1, char(kGenerator))));
}

std::string privateKey(std::string_view user, std::string_view pin, std::string_view salt) {
    return sha512(std::string(salt) + sha512(std::string(user) + ":" + std::string(pin)));
}

std::string proofOf(std::string_view user, std::string_view salt, std::string_view a, std::string_view b, std::string_view key) {
    const std::string hn = sha512(srpPrime());
    const std::string hg = sha512(std::string(1, char(kGenerator)));
    std::string mixed(hn.size(), '\0');
    for (size_t i = 0; i < hn.size(); ++i) mixed[i] = char(hn[i] ^ hg[i]);
    return sha512(mixed + sha512(user) + std::string(salt) + std::string(a) + std::string(b) + std::string(key));
}

std::string littleEndian(uint64_t value, size_t bytes) {
    std::string out;
    for (size_t i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xFF));
    return out;
}

std::string reversed(std::string_view bytes) {
    return {bytes.rbegin(), bytes.rend()};
}

uint32_t word(std::string_view bytes, size_t at) {
    return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 | uint32_t(uint8_t(bytes[at + 2])) << 16 |
           uint32_t(uint8_t(bytes[at + 3])) << 24;
}

uint32_t rotate(uint32_t v, int c) {
    return v << c | v >> (32 - c);
}

std::array<uint32_t, 16> chachaBlock(std::string_view key, uint32_t counter, std::string_view nonce) {
    std::array<uint32_t, 16> state{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (size_t i = 0; i < 8; ++i) state[4 + i] = word(key, 4 * i);
    state[12] = counter;
    for (size_t i = 0; i < 3; ++i) state[13 + i] = word(nonce, 4 * i);
    auto x = state;
    const auto quarter = [&x](size_t a, size_t b, size_t c, size_t d) {
        x[a] += x[b];
        x[d] = rotate(x[d] ^ x[a], 16);
        x[c] += x[d];
        x[b] = rotate(x[b] ^ x[c], 12);
        x[a] += x[b];
        x[d] = rotate(x[d] ^ x[a], 8);
        x[c] += x[d];
        x[b] = rotate(x[b] ^ x[c], 7);
    };
    for (int round = 0; round < 10; ++round) {
        quarter(0, 4, 8, 12);
        quarter(1, 5, 9, 13);
        quarter(2, 6, 10, 14);
        quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15);
        quarter(1, 6, 11, 12);
        quarter(2, 7, 8, 13);
        quarter(3, 4, 9, 14);
    }
    for (size_t i = 0; i < x.size(); ++i) x[i] += state[i];
    return x;
}

std::string padTo16(std::string_view data) {
    std::string out(data);
    out.resize((out.size() + 15) / 16 * 16, '\0');
    return out;
}

std::string tagOf(std::string_view key, std::string_view nonce, std::string_view aad, std::string_view cipher) {
    const auto block = chachaBlock(key, 0, nonce);
    std::string oneTimeKey;
    for (size_t i = 0; i < 8; ++i) oneTimeKey += littleEndian(block[i], 4);
    return poly1305(oneTimeKey, padTo16(aad) + padTo16(cipher) + littleEndian(aad.size(), 8) + littleEndian(cipher.size(), 8));
}

std::string nonceOf(uint64_t count) {
    return std::string(4, '\0') + littleEndian(count, 8);
}

} // namespace

std::string randomBytes(size_t size) {
    std::string out(size, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), ULONG(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("no random numbers");
    return out;
}

std::string sha512(std::string_view data) {
    return hashWith(BCRYPT_SHA512_ALG_HANDLE, {}, data);
}

std::string hmacSha512(std::string_view key, std::string_view data) {
    return hashWith(BCRYPT_HMAC_SHA512_ALG_HANDLE, key, data);
}

std::string hkdfSha512(std::string_view secret, std::string_view salt, std::string_view info, size_t size) {
    const std::string key = hmacSha512(salt, secret);
    std::string out, block;
    for (uint8_t i = 1; out.size() < size; ++i) {
        block.append(info);
        block += char(i);
        block = hmacSha512(key, block);
        out += block;
    }
    out.resize(size);
    return out;
}

std::string chacha20(std::string_view key, uint32_t counter, std::string_view nonce, std::string_view data) {
    std::string out(data);
    for (size_t at = 0; at < out.size(); at += 64, ++counter) {
        const auto block = chachaBlock(key, counter, nonce);
        for (size_t i = at; i < std::min(at + 64, out.size()); ++i)
            out[i] = char(uint8_t(out[i]) ^ uint8_t(block[(i - at) / 4] >> ((i - at) % 4 * 8)));
    }
    return out;
}

std::string poly1305(std::string_view key, std::string_view message) {
    static const Montgomery prime(subtract(Limbs{0, 0, 0, 0, 4}, {5}));
    std::string clamped(key.substr(0, 16));
    for (const size_t i : {3, 7, 11, 15}) clamped[i] = char(uint8_t(clamped[i]) & 0x0F);
    for (const size_t i : {4, 8, 12}) clamped[i] = char(uint8_t(clamped[i]) & 0xFC);
    const Limbs r = fromBigEndian(reversed(clamped));
    Limbs accumulator;
    for (size_t at = 0; at < message.size(); at += 16) {
        const Limbs n = fromBigEndian(reversed(std::string(message.substr(at, 16)) + '\x01'));
        accumulator = prime.product(reduce(add(accumulator, n), prime.modulus()), r);
    }
    accumulator = add(accumulator, fromBigEndian(reversed(key.substr(16, 16))));
    accumulator.resize(4, 0);
    std::string tag;
    for (const uint32_t limb : accumulator) tag += littleEndian(limb, 4);
    return tag;
}

std::string seal(std::string_view key, std::string_view nonce, std::string_view aad, std::string_view plain) {
    const std::string cipher = chacha20(key, 1, nonce, plain);
    return cipher + tagOf(key, nonce, aad, cipher);
}

std::optional<std::string> open(std::string_view key, std::string_view nonce, std::string_view aad, std::string_view sealed) {
    if (sealed.size() < kTagBytes) return std::nullopt;
    const std::string_view cipher = sealed.substr(0, sealed.size() - kTagBytes);
    const std::string expected = tagOf(key, nonce, aad, cipher);
    uint8_t differs = 0;
    for (size_t i = 0; i < kTagBytes; ++i) differs |= uint8_t(expected[i] ^ sealed[cipher.size() + i]);
    if (differs) return std::nullopt;
    return chacha20(key, 1, nonce, cipher);
}

std::string tlv(const TlvItems& items) {
    std::string out;
    for (const auto& [type, value] : items) {
        size_t at = 0;
        do {
            const size_t size = std::min<size_t>(value.size() - at, 255);
            out += char(type);
            out += char(size);
            out += value.substr(at, size);
            at += size;
        } while (at < value.size());
    }
    return out;
}

std::map<uint8_t, std::string> parseTlv(std::string_view data) {
    std::map<uint8_t, std::string> items;
    for (size_t at = 0; at + 2 <= data.size();) {
        const auto type = uint8_t(data[at]);
        const size_t size = std::min<size_t>(uint8_t(data[at + 1]), data.size() - at - 2);
        items[type] += data.substr(at + 2, size);
        at += 2 + size;
    }
    return items;
}

std::string modPow(std::string_view base, std::string_view exponent, std::string_view modulus) {
    return toBigEndian(Montgomery(fromBigEndian(modulus)).power(fromBigEndian(base), exponent));
}

const std::string& srpPrime() {
    static const std::string prime = [] {
        constexpr std::string_view hex =
            "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
            "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
            "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
            "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
            "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
            "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
            "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
            "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
        std::string bytes;
        for (size_t i = 0; i < hex.size(); i += 2) bytes.push_back(char(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
        return bytes;
    }();
    return prime;
}

SrpProof srpClient(std::string_view user, std::string_view pin, std::string_view salt, std::string_view receiverKey,
                   std::string_view secret) {
    const Montgomery& group = srpGroup();
    const Limbs b = fromBigEndian(receiverKey);
    if (b.empty() || compare(reduce(b, group.modulus()), {}) == 0) return {};
    const Limbs a = group.power({kGenerator}, secret);
    const std::string aBytes = toBigEndian(a);
    const std::string bBytes(receiverKey);
    const Limbs u = hashNumber(padded(aBytes) + padded(bBytes));
    const Limbs x = fromBigEndian(privateKey(user, pin, salt));
    const Limbs kv = group.product(fromBigEndian(multiplier()), group.power({kGenerator}, toBigEndian(x)));
    const Limbs reduced = reduce(b, group.modulus());
    const Limbs base = compare(reduced, kv) >= 0 ? subtract(reduced, kv) : subtract(add(reduced, group.modulus()), kv);
    const std::string exponent = toBigEndian(add(fromBigEndian(secret), multiply(u, x)));
    const std::string key = sha512(toBigEndian(group.power(base, exponent)));
    SrpProof proof{aBytes, proofOf(user, salt, aBytes, bBytes, key), key, {}};
    proof.expected = sha512(aBytes + proof.proof + key);
    return proof;
}

SrpServer srpServer(std::string_view user, std::string_view pin, std::string_view salt, std::string_view secret) {
    const Montgomery& group = srpGroup();
    const Limbs v = group.power({kGenerator}, privateKey(user, pin, salt));
    const Limbs kv = group.product(fromBigEndian(multiplier()), v);
    const Limbs b = reduce(add(kv, group.power({kGenerator}, secret)), group.modulus());
    return {toBigEndian(b), toBigEndian(v)};
}

std::optional<std::pair<std::string, std::string>> srpServerCheck(const SrpServer& server, std::string_view user,
                                                                  std::string_view salt, std::string_view senderKey,
                                                                  std::string_view proof, std::string_view secret) {
    const Montgomery& group = srpGroup();
    const Limbs a = reduce(fromBigEndian(senderKey), group.modulus());
    if (a.empty()) return std::nullopt;
    const Limbs u = hashNumber(padded(toBigEndian(a)) + padded(server.publicKey));
    const Limbs base = group.product(a, group.power(fromBigEndian(server.verifier), toBigEndian(u)));
    const std::string key = sha512(toBigEndian(group.power(base, secret)));
    const std::string aBytes = toBigEndian(a);
    if (proofOf(user, salt, aBytes, server.publicKey, key) != proof) return std::nullopt;
    return std::pair(key, sha512(aBytes + std::string(proof) + key));
}

SecureChannel SecureChannel::forSender(std::string_view sessionKey) {
    return {hkdfSha512(sessionKey, "Control-Salt", "Control-Write-Encryption-Key", 32),
            hkdfSha512(sessionKey, "Control-Salt", "Control-Read-Encryption-Key", 32)};
}

SecureChannel SecureChannel::forReceiver(std::string_view sessionKey) {
    return {hkdfSha512(sessionKey, "Control-Salt", "Control-Read-Encryption-Key", 32),
            hkdfSha512(sessionKey, "Control-Salt", "Control-Write-Encryption-Key", 32)};
}

std::string SecureChannel::seal(std::string_view plain) {
    std::string out;
    for (size_t at = 0; at < plain.size(); at += kBlockBytes) {
        const std::string_view block = plain.substr(at, kBlockBytes);
        const std::string aad = littleEndian(block.size(), 2);
        out += aad + airplay::seal(writeKey_, nonceOf(written_++), aad, block);
    }
    return out;
}

std::optional<std::string> SecureChannel::open(std::string& received) {
    std::string plain;
    while (received.size() >= 2) {
        const size_t size = size_t(uint8_t(received[0])) | size_t(uint8_t(received[1])) << 8;
        if (received.size() < 2 + size + kTagBytes) break;
        const auto block = airplay::open(readKey_, nonceOf(read_++), received.substr(0, 2), received.substr(2, size + kTagBytes));
        if (!block) return std::nullopt;
        plain += *block;
        received.erase(0, 2 + size + kTagBytes);
    }
    return plain;
}

} // namespace camel::server::airplay
