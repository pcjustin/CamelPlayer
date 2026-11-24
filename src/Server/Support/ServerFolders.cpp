// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/ServerFolders.h"

#include <windows.h>
#include <lm.h>
#include <shlwapi.h>

#include <algorithm>
#include <stdexcept>

#include "Common/util/Text.h"

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace camel::server {

namespace {

std::vector<ServerFolder> drives() {
    std::vector<ServerFolder> out;
    std::wstring letters(512, L'\0');
    letters.resize(GetLogicalDriveStringsW(DWORD(letters.size()), letters.data()));
    for (const wchar_t* root = letters.c_str(); *root; root += wcslen(root) + 1) {
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        std::wstring name(root, 2);
        wchar_t label[MAX_PATH + 1] = {};
        // A mapped drive's label is not asked for: its computer may be asleep.
        if (type != DRIVE_REMOTE && GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) &&
            *label)
            name = std::wstring(label).append(L" (").append(name).append(L")");
        out.push_back({toUtf8(name), toUtf8(root)});
    }
    return out;
}

// `\\computer` alone names a computer, whose shares are listed rather than its folders.
bool namesComputer(const std::wstring& path) {
    return path.size() > 2 && path.starts_with(L"\\\\") && path.find_first_of(L"\\/", 2) == std::wstring::npos;
}

std::vector<ServerFolder> shares(const std::wstring& computer) {
    SHARE_INFO_1* info = nullptr;
    DWORD read = 0, total = 0;
    const NET_API_STATUS status = NetShareEnum(const_cast<wchar_t*>(computer.c_str()), 1, reinterpret_cast<BYTE**>(&info),
                                               MAX_PREFERRED_LENGTH, &read, &total, nullptr);
    if (status != NERR_Success && status != ERROR_MORE_DATA)
        throw std::runtime_error("the shares of " + toUtf8(computer) + " cannot be listed (" + std::to_string(status) + ")");
    std::vector<ServerFolder> out;
    for (DWORD i = 0; i < read; ++i) {
        const std::wstring name = info[i].shi1_netname;
        if ((info[i].shi1_type & STYPE_MASK) != STYPE_DISKTREE || (info[i].shi1_type & STYPE_SPECIAL) || name.ends_with(L'$'))
            continue;
        out.push_back({toUtf8(name), toUtf8(std::wstring(computer).append(1, L'\\').append(name))});
    }
    NetApiBufferFree(info);
    return out;
}

std::vector<ServerFolder> folders(std::wstring path) {
    if (!path.ends_with(L'\\') && !path.ends_with(L'/')) path += L'\\';
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileExW((path + L"*").c_str(), FindExInfoBasic, &found, FindExSearchLimitToDirectories, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        throw std::runtime_error(toUtf8(path) + " cannot be listed (" + std::to_string(GetLastError()) + ")");
    std::vector<ServerFolder> out;
    do {
        const std::wstring name = found.cFileName;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L"..") continue;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        out.push_back({toUtf8(name), toUtf8(path + name)});
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return out;
}

} // namespace

std::vector<ServerFolder> foldersIn(const std::string& pathUtf8) {
    const std::wstring path = toWide(pathUtf8);
    auto out = path.empty() ? drives() : namesComputer(path) ? shares(path) : folders(path);
    if (!path.empty())
        std::ranges::sort(out, [](const ServerFolder& a, const ServerFolder& b) {
            return StrCmpLogicalW(toWide(a.name).c_str(), toWide(b.name).c_str()) < 0;
        });
    return out;
}

} // namespace camel::server
