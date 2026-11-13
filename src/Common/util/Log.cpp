// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/Log.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace camel::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LocalTime (*g_clock)() = nullptr;
std::filesystem::path g_directory;
std::wstring g_prefix;
unsigned g_day = 0; // the open file's date, as YYYYMMDD

LocalTime now() {
    if (g_clock) return g_clock();
    SYSTEMTIME t;
    GetLocalTime(&t);
    return {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds};
}

unsigned dateOf(const LocalTime& t) {
    return t.year * 10000 + t.month * 100 + t.day;
}

// Deletes logs older than 7 days and opens the day's file. g_mutex is held.
void openDay(const LocalTime& t) {
    std::error_code ec;
    std::filesystem::create_directories(g_directory, ec);
    auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (auto& e : std::filesystem::directory_iterator(g_directory, ec))
        if (e.is_regular_file(ec) && e.path().extension() == L".log" && e.last_write_time(ec) < cutoff)
            std::filesystem::remove(e.path(), ec);
    if (g_file && fclose(g_file) != 0) OutputDebugStringA("log: closing previous log file failed\n");
    g_file = nullptr;
    g_day = dateOf(t);
    wchar_t name[128];
    const int nameLength = swprintf(name, 128, L"%s-%04u%02u%02u.log", g_prefix.c_str(), t.year, t.month, t.day);
    if (nameLength < 0 || size_t(nameLength) >= std::size(name)) return;
    g_file = _wfsopen((g_directory / name).c_str(), L"a", _SH_DENYNO);
}

void write(const char* level, const char* fmt, va_list ap) {
    char msg[2048];
    const int msgLength = vsnprintf(msg, sizeof msg, fmt, ap);
    if (msgLength < 0) strcpy_s(msg, "log formatting failed");
    const LocalTime t = now();
    char line[2200];
    const int lineLength = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u [%s] (%lu) %s\n", t.hour, t.minute, t.second,
                                    t.millisecond, level, GetCurrentThreadId(), msg);
    if (lineLength < 0) strcpy_s(line, "log line formatting failed\n");
    OutputDebugStringA(line);
    std::lock_guard lock(g_mutex);
    // A program left running past midnight goes on in the new day's file, and old logs go as they would at a start.
    if (g_file && dateOf(t) != g_day) openDay(t);
    if (g_file) {
        const bool writeFailed = fputs(line, g_file) == EOF;
        const bool flushFailed = fflush(g_file) != 0;
        if (writeFailed || flushFailed) {
            FILE* failedFile = std::exchange(g_file, nullptr);
            if (fclose(failedFile) != 0) OutputDebugStringA("log: closing failed log file failed\n");
        }
    }
}

} // namespace

void init(const std::filesystem::path& logDir, const wchar_t* prefix) {
    const LocalTime t = now();
    std::lock_guard lock(g_mutex);
    g_directory = logDir;
    g_prefix = prefix;
    openDay(t);
}

void setClockForTest(LocalTime (*clock)()) {
    std::lock_guard lock(g_mutex);
    g_clock = clock;
}

void info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("info", fmt, ap);
    va_end(ap);
}

void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("warn", fmt, ap);
    va_end(ap);
}

void error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("error", fmt, ap);
    va_end(ap);
}

} // namespace camel::log
