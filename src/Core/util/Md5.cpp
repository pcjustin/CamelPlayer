#include "util/Md5.h"

#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace camel {

Md5::Md5() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("BCrypt MD5 unavailable");
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    alg_ = alg;
    hash_ = hash;
}

Md5::~Md5() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
}

void Md5::update(const void* data, size_t len) {
    auto p = static_cast<const UCHAR*>(data);
    while (len) {
        ULONG n = len > 0x40000000 ? 0x40000000 : ULONG(len);
        BCryptHashData(hash_, const_cast<PUCHAR>(p), n, 0);
        p += n;
        len -= n;
    }
}

std::array<uint8_t, 16> Md5::finish() {
    std::array<uint8_t, 16> d{};
    BCryptFinishHash(hash_, d.data(), 16, 0);
    return d;
}

std::string toHex(const std::array<uint8_t, 16>& digest) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : digest) {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}

} // namespace camel
