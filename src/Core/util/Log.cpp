#include "util/Log.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace camel::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

void write(const char* level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof line, "%02u:%02u:%02u.%03u [%s] (%lu) %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level,
             GetCurrentThreadId(), msg);
    OutputDebugStringA(line);
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}

}  // namespace

void init(const std::filesystem::path& logDir, const wchar_t* prefix) {
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (auto& e : std::filesystem::directory_iterator(logDir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == L".log" && e.last_write_time(ec) < cutoff) std::filesystem::remove(e.path(), ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[128];
    swprintf(name, 128, L"%s-%04u%02u%02u.log", prefix, t.wYear, t.wMonth, t.wDay);
    std::lock_guard lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = _wfsopen((logDir / name).c_str(), L"a", _SH_DENYNO);
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

}  // namespace camel::log
