// MD5 via Windows CNG (BCrypt).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace camel {

class Md5 {
public:
    Md5();
    ~Md5();
    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void update(const void* data, size_t len);
    std::array<uint8_t, 16> finish();

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};

std::string toHex(const std::array<uint8_t, 16>& digest);

}  // namespace camel
