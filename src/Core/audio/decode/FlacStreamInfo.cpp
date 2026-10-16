#include "audio/decode/FlacStreamInfo.h"

#include <windows.h>

#include <algorithm>

#include "util/Text.h"

namespace camel::audio {

bool readFlacStreamInfo(const std::filesystem::path& path, SourceInfo& out) {
    HANDLE h = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t off, void* buf, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(off);
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
    };
    uint8_t hdr[10];
    uint64_t pos = 0;
    bool ok = false;
    if (readAt(0, hdr, 10)) {
        if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') // ID3v2: synchsafe size
            pos = 10 + ((uint64_t(hdr[6] & 0x7f) << 21) | (uint64_t(hdr[7] & 0x7f) << 14) | (uint64_t(hdr[8] & 0x7f) << 7) |
                        uint64_t(hdr[9] & 0x7f));
        uint8_t b[4 + 4 + 34]; // "fLaC", block header, STREAMINFO body
        if (readAt(pos, b, sizeof b) && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C' && (b[4] & 0x7f) == 0 &&
            b[5] == 0 && b[6] == 0 && b[7] == 34) {
            const uint8_t* s = b + 8;
            SourceInfo parsed;
            parsed.sampleRate = (uint32_t(s[10]) << 12) | (uint32_t(s[11]) << 4) | (s[12] >> 4);
            parsed.channels = ((s[12] >> 1) & 7) + 1;
            parsed.bitsPerSample = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
            parsed.totalFrames = (uint64_t(s[13] & 0x0f) << 32) | (uint64_t(s[14]) << 24) | (uint64_t(s[15]) << 16) |
                                 (uint64_t(s[16]) << 8) | s[17];
            std::array<uint8_t, 16> md5;
            std::copy(s + 18, s + 34, md5.begin());
            if (std::any_of(md5.begin(), md5.end(), [](uint8_t x) { return x != 0; })) parsed.md5 = md5;
            if (parsed.sampleRate && parsed.bitsPerSample >= 4) {
                out = parsed;
                ok = true;
            }
        }
    }
    CloseHandle(h);
    return ok;
}

} // namespace camel::audio
