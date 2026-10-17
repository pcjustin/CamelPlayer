// bench-decoders / bench-one: measurements for docs/Decoder-comparison.md (docs/Plan.md §16.5).
// Every decoder x file pair runs in its own child process so peak memory is not shared.
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "Commands.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/testing/TestAudio.h"
#include "Engine/verify/FlacMd5.h"
#include "Common/util/Text.h"

#pragma comment(lib, "psapi.lib")

using namespace camel;
using namespace camel::audio;
using nlohmann::json;

namespace camelcli {

namespace {

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double cpuMs() {
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 10000.0; };
    return v(k) + v(u);
}

double peakMB() {
    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize / 1048576.0;
}

double workingMB() {
    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.WorkingSetSize / 1048576.0;
}

} // namespace

// bench-one <decoder> <file>: prints one JSON object.
int cmdBenchOne(const Args& args) {
    auto pos = positional(args);
    if (pos.size() < 2) return 2;
    DecoderKind kind;
    if (!decoderFromId(toUtf8(pos[0]), kind)) return 2;
    const std::filesystem::path file = pos[1];
    json r;
    r["decoder"] = decoderName(kind);
    r["file"] = toUtf8(file.filename().wstring());
    const double baseMB = workingMB();
    std::unique_ptr<IDecoder> dec;
    double t0 = nowMs();
    try {
        dec = openDecoder(kind, file);
    } catch (const Error& e) {
        r["openError"] = std::string(errorCodeName(e.code())) + ": " + e.what();
        std::printf("%s\n", r.dump().c_str());
        return 0;
    }
    r["openMs"] = nowMs() - t0;
    const SourceInfo in = dec->info();
    r["rate"] = in.sampleRate;
    r["bits"] = in.bitsPerSample;
    r["channels"] = in.channels;
    r["totalFrames"] = in.totalFrames;

    // 1. Full sequential decode: speed, CPU, memory, MD5
    FlacMd5 md5(in.bitsPerSample);
    std::vector<int32_t> buf(4096 * size_t(in.channels));
    uint64_t frames = 0;
    double c0 = cpuMs();
    t0 = nowMs();
    try {
        while (size_t n = dec->read(buf.data(), 4096)) {
            md5.update(buf.data(), n * in.channels);
            frames += n;
        }
    } catch (const Error& e) {
        r["decodeError"] = std::string(errorCodeName(e.code())) + ": " + e.what();
    }
    r["decodeMs"] = nowMs() - t0;
    r["cpuMs"] = cpuMs() - c0;
    r["framesDecoded"] = frames;
    r["peakMB"] = peakMB() - baseMB;
    auto digest = md5.finish();
    r["md5"] = toHex(digest);
    r["md5Status"] = !in.md5 ? "none" : (*in.md5 == digest ? "ok" : "mismatch");

    // 2. Seek accuracy against the libFLAC reference (only for files that decode cleanly)
    if (!r.contains("decodeError") && frames > 8192) {
        auto ref = testaudio::referenceDecode(file);
        if (ref.error.empty()) {
            const size_t ch = in.channels;
            uint64_t lcg = 12345;
            int exact = 0, total = 0, failed = 0;
            json misses = json::array();
            double sumMs = 0, maxMs = 0;
            for (int i = 0; i < 20; i++) {
                lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
                uint64_t target = (lcg >> 20) % (frames - 4096);
                if (i == 0) target = 0;
                if (i == 1) target = frames - 4096;
                double s0 = nowMs();
                try {
                    dec->seek(target);
                    size_t got = 0;
                    while (got < 4096) {
                        size_t n = dec->read(buf.data() + got * ch, 4096 - got);
                        if (!n) break;
                        got += n;
                    }
                    double ms = nowMs() - s0;
                    sumMs += ms;
                    maxMs = std::max(maxMs, ms);
                    total++;
                    if (got == 4096 && std::equal(buf.begin(), buf.begin() + 4096 * ch, ref.samples.begin() + target * ch))
                        exact++;
                    else {
                        // find how far off the decoder landed: compare against the reference around the target
                        long long offset = 0;
                        for (long long d = -8192; d <= 8192 && got >= 16; d++) {
                            long long at = (long long)target + d;
                            if (at < 0 || uint64_t(at) + 16 > ref.samples.size() / ch) continue;
                            if (std::equal(buf.begin(), buf.begin() + 16 * ch, ref.samples.begin() + at * ch)) {
                                offset = d;
                                break;
                            }
                        }
                        misses.push_back({{"target", target}, {"got", got}, {"offsetFrames", offset}});
                    }
                } catch (const Error& e) {
                    failed++;
                    misses.push_back({{"target", target}, {"error", e.what()}});
                }
            }
            r["seek"] = {
                {"tried", 20}, {"exact", exact}, {"failed", failed}, {"avgMs", total ? sumMs / total : 0.0}, {"maxMs", maxMs}};
            if (!misses.empty()) r["seek"]["misses"] = misses;
        }
    }
    std::printf("%s\n", r.dump().c_str());
    return 0;
}

// bench-decoders <dir|files...> [--runs N] [--out results.json]
int cmdBenchDecoders(const Args& args) {
    std::vector<std::filesystem::path> files;
    for (auto& p : positional(args)) {
        if (std::filesystem::is_directory(p)) {
            for (auto& e : std::filesystem::directory_iterator(p))
                if (e.path().extension() == L".flac") files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::puts("usage: bench-decoders <dir|files...> [--runs N] [--out results.json]");
        return 2;
    }
    const int runs = option(args, L"--runs") ? std::stoi(*option(args, L"--runs")) : 3;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    json all = json::array();
    for (auto& f : files) {
        for (DecoderKind k : decodersFor(probeFormat(f))) {
            std::vector<json> results;
            for (int run = 0; run < runs; run++) {
                std::wstring cmd = L"\"" + std::wstring(self) + L"\" bench-one " + toWide(decoderId(k)) + L" \"" +
                                   std::filesystem::absolute(f).wstring() + L"\"";
                SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
                HANDLE rd, wr;
                CreatePipe(&rd, &wr, &sa, 0);
                SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW si{sizeof si};
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = wr;
                si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
                PROCESS_INFORMATION pi{};
                if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
                    throw std::runtime_error("CreateProcess failed");
                CloseHandle(wr);
                std::string out;
                char chunk[4096];
                DWORD got;
                while (ReadFile(rd, chunk, sizeof chunk, &got, nullptr) && got) out.append(chunk, got);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                CloseHandle(rd);
                json j;
                try {
                    j = json::parse(out);
                } catch (...) {
                    j = {{"decoder", decoderName(k)},
                         {"file", toUtf8(f.filename().wstring())},
                         {"crash", true},
                         {"exitCode", code}};
                }
                results.push_back(j);
            }
            // keep the run with the median decode time
            std::sort(results.begin(), results.end(),
                      [](const json& a, const json& b) { return a.value("decodeMs", 0.0) < b.value("decodeMs", 0.0); });
            json best = results[results.size() / 2];
            best["runs"] = runs;
            std::printf("%-26s %-10s ", toUtf8(f.filename().wstring()).c_str(), decoderName(k));
            if (best.contains("crash"))
                std::printf("CRASH (exit 0x%08lX)\n", (unsigned long)best.value("exitCode", 0u));
            else if (best.contains("openError"))
                std::printf("open: %s\n", best["openError"].get<std::string>().c_str());
            else {
                double secs = framesToSeconds(best.value("framesDecoded", 0ull), best.value("rate", 1u));
                std::printf("%8.1f ms (%5.0fx) cpu %8.1f ms  mem %6.1f MB  md5 %-8s", best.value("decodeMs", 0.0),
                            secs / (best.value("decodeMs", 1.0) / 1000), best.value("cpuMs", 0.0), best.value("peakMB", 0.0),
                            best.value("md5Status", "").c_str());
                if (best.contains("decodeError"))
                    std::printf(" error after %llu frames: %s", best.value("framesDecoded", 0ull),
                                best["decodeError"].get<std::string>().c_str());
                if (best.contains("seek"))
                    std::printf(" seek %d/%d exact, avg %.2f ms", best["seek"].value("exact", 0), best["seek"].value("tried", 0),
                                best["seek"].value("avgMs", 0.0));
                std::puts("");
            }
            all.push_back(best);
        }
    }
    if (auto out = option(args, L"--out")) {
        std::ofstream o(std::filesystem::path(*out), std::ios::binary);
        o << all.dump(2);
    }
    return 0;
}

} // namespace camelcli
