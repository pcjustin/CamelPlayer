// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/CrashDump.h"

#include <windows.h>
#include <dbghelp.h>

#include <iterator>

#pragma comment(lib, "dbghelp.lib")

namespace camel {
namespace {

wchar_t g_dir[MAX_PATH];

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 64];
    const int pathLength = swprintf(path, MAX_PATH + 64, L"%s\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dir, t.wYear, t.wMonth,
                                    t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (pathLength < 0 || size_t(pathLength) >= std::size(path)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithThreadInfo, &info, nullptr, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void installCrashDumps(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onCrash);
}

} // namespace camel
