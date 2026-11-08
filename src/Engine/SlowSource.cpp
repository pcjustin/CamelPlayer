// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/SlowSource.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace camel::audio {
namespace {

// Unbuffered reads must start and end on sector boundaries; 64 KiB covers every sector size.
constexpr uint64_t kAlignment = uint64_t{64} * 1024;
constexpr DWORD kChunk = 1024 * 1024;
// The read rate varies; the buffer allows for it to fall this much below what was measured.
constexpr double kMargin = 1.08;
constexpr double kSpareSeconds = 2.0;

struct Handle {
    HANDLE h;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

} // namespace

bool onNetworkShare(const std::wstring& path) {
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\") || p.starts_with(L"\\\\?\\unc\\")) return true;
    if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    if (p.starts_with(L"\\\\")) return true;
    if (p.size() < 3 || p[1] != L':') return false;
    return GetDriveTypeW(p.substr(0, 3).c_str()) == DRIVE_REMOTE;
}

std::optional<double> measureReadRate(const std::wstring& path, uint64_t offset, uint64_t bytes) {
    const Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) return std::nullopt;
    void* buffer = VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return std::nullopt;
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(offset / kAlignment * kAlignment);
    uint64_t read = 0;
    const auto start = std::chrono::steady_clock::now();
    if (SetFilePointerEx(file.h, at, nullptr, FILE_BEGIN)) {
        while (read < bytes) {
            DWORD got = 0;
            if (!ReadFile(file.h, buffer, kChunk, &got, nullptr) || !got) break;
            read += got;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    VirtualFree(buffer, 0, MEM_RELEASE);
    if (read < std::min<uint64_t>(bytes, kChunk) || seconds <= 0) return std::nullopt;
    return double(read) / seconds;
}

double bufferSeconds(double remainingSeconds, double fillRatio) {
    if (fillRatio >= kFastEnough || remainingSeconds <= 0) return 0.0;
    return remainingSeconds * std::max(0.0, 1.0 - fillRatio / kMargin) + kSpareSeconds;
}

} // namespace camel::audio
