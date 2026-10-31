// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/CrashDump.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>

#include "Common/util/Log.h"

#pragma comment(lib, "dbghelp.lib")

namespace camel {
namespace {

wchar_t g_dir[MAX_PATH];
std::atomic_flag g_fatalDumped;

struct DumpRequest {
    const wchar_t* suffix;
    DWORD threadId;
    EXCEPTION_POINTERS* exception;
};

DWORD WINAPI writeDump(void* parameter) {
    const auto& request = *static_cast<const DumpRequest*>(parameter);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 64];
    const int pathLength = swprintf(path, MAX_PATH + 64, L"%s\\crash-%04u%02u%02u-%02u%02u%02u%s.dmp", g_dir, t.wYear, t.wMonth,
                                    t.wDay, t.wHour, t.wMinute, t.wSecond, request.suffix);
    if (pathLength < 0 || size_t(pathLength) >= std::size(path)) return 1;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION info{request.threadId, request.exception, FALSE};
    // The memory the stacks point at holds the paths and names being worked on, small enough to send.
    const auto type =
        MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, request.exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return 0;
}

// A thread of its own writes the dump: a thread that overflowed its stack has none left to write it with.
void dumpFrom(const wchar_t* suffix, EXCEPTION_POINTERS* exception) {
    DumpRequest request{suffix, GetCurrentThreadId(), exception};
    if (HANDLE writer = CreateThread(nullptr, 0, writeDump, &request, 0, nullptr)) {
        WaitForSingleObject(writer, INFINITE);
        CloseHandle(writer);
    }
}

void dumpFatal(const wchar_t* suffix, EXCEPTION_POINTERS* exception) {
    if (!g_fatalDumped.test_and_set()) dumpFrom(suffix, exception);
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    dumpFatal(L"", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), std::terminate and CRT checks end the process without an unhandled exception reaching onCrash.
void onAbort(int) {
    dumpFatal(L"-abort", nullptr);
}

void onPureCall() {
    dumpFatal(L"-purecall", nullptr);
    std::abort();
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    dumpFatal(L"-invalid-parameter", nullptr);
    std::abort();
}

double privateMegabytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters))
        return 0;
    return double(counters.PrivateUsage) / (1024 * 1024);
}

} // namespace

void pruneCrashDumps(const std::filesystem::path& dir, size_t keep) {
    std::error_code ec;
    std::vector<std::filesystem::path> dumps;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(L"crash-") && entry.path().extension() == L".dmp") dumps.push_back(entry.path());
    }
    if (dumps.size() <= keep) return;
    // crash-YYYYMMDD-hhmmss[-reason].dmp sorts by time.
    std::sort(dumps.begin(), dumps.end());
    for (size_t i = 0; i + keep < dumps.size(); ++i) std::filesystem::remove(dumps[i], ec);
}

void installCrashDumps(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    pruneCrashDumps(dir, kKeptCrashDumps);
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onCrash);
    signal(SIGABRT, onAbort);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
}

HangWatch::HangWatch(std::function<bool(std::function<void()>)> post, std::chrono::seconds timeout)
    : thread_([post = std::move(post), timeout](const std::stop_token& stop) {
          constexpr auto kTick = std::chrono::seconds(1);
          constexpr double kMemoryStepMegabytes = 1024;
          auto answered = std::make_shared<std::atomic<bool>>(true);
          int64_t unanswered = 0;
          bool reported = false;
          double nextMemoryReport = kMemoryStepMegabytes;
          while (!stop.stop_requested()) {
              // Ticks rather than clock time, so a sleeping PC does not count as a hang.
              if (answered->exchange(false)) {
                  if (reported) log::warn("hang: UI thread answers again after %lld s", static_cast<long long>(unanswered));
                  unanswered = 0;
                  reported = false;
                  if (!post([answered] { answered->store(true); })) return;
              } else if (++unanswered >= timeout / kTick && !reported) {
                  reported = true;
                  log::error("hang: UI thread has not answered for %lld s; %.0f MB private memory; writing a dump",
                             static_cast<long long>(unanswered), privateMegabytes());
                  dumpFrom(L"-hang", nullptr);
              }
              if (const double megabytes = privateMegabytes(); megabytes >= nextMemoryReport) {
                  log::warn("memory: %.0f MB private memory", megabytes);
                  while (nextMemoryReport <= megabytes) nextMemoryReport += kMemoryStepMegabytes;
              }
              std::this_thread::sleep_for(kTick);
          }
      }) {}

} // namespace camel
