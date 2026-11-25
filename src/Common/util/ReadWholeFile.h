// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace camel {

// The whole of `file`, or nothing when it cannot be read. Another may hold a file just put in place for a moment, as
// the one renaming it there or a virus scanner looking at it: the read waits up to two seconds for it rather than
// finding nothing.
inline std::optional<std::string> readWholeFile(const std::filesystem::path& file) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100 && handle == INVALID_HANDLE_VALUE; ++attempt) {
        handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        if (const DWORD error = GetLastError(); error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
            return std::nullopt;
        Sleep(20);
    }
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string bytes;
    char buffer[65536];
    DWORD got = 0;
    bool read = true;
    while ((read = ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) != FALSE) && got > 0) bytes.append(buffer, got);
    CloseHandle(handle);
    if (!read) return std::nullopt;
    return bytes;
}

} // namespace camel
