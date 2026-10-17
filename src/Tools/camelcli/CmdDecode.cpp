#include <chrono>
#include <cstdio>
#include <vector>

#include "Commands.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/verify/FlacMd5.h"
#include "Common/util/Text.h"

using namespace camel;
using namespace camel::audio;

namespace camelcli {

std::optional<std::wstring> option(const Args& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == name) return args[i + 1];
    return std::nullopt;
}

bool flag(const Args& args, const wchar_t* name) {
    for (auto& a : args)
        if (a == name) return true;
    return false;
}

std::vector<std::wstring> positional(const Args& args) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind(L"--", 0) == 0) {
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) i++;
            continue;
        }
        out.push_back(args[i]);
    }
    return out;
}

int cmdDecode(const Args& args) {
    auto files = positional(args);
    if (files.empty()) {
        std::puts("usage: decode <file.flac> [--decoder id|all]");
        return 2;
    }
    std::wstring which = option(args, L"--decoder").value_or(L"all");
    std::vector<DecoderKind> kinds;
    if (which == L"all")
        kinds.assign(std::begin(kAllDecoders), std::end(kAllDecoders));
    else {
        DecoderKind k;
        if (!decoderFromId(toUtf8(which), k)) throw std::runtime_error("unknown decoder " + toUtf8(which));
        kinds.push_back(k);
    }
    int failures = 0;
    for (auto& file : files) {
        std::printf("%s\n", toUtf8(file).c_str());
        for (DecoderKind k : kinds) {
            auto t0 = std::chrono::steady_clock::now();
            try {
                auto dec = openDecoder(k, file);
                const auto& in = dec->info();
                FlacMd5 md5(in.bitsPerSample);
                std::vector<int32_t> buf(4096 * in.channels);
                uint64_t frames = 0;
                while (size_t n = dec->read(buf.data(), 4096)) {
                    md5.update(buf.data(), n * in.channels);
                    frames += n;
                }
                auto digest = md5.finish();
                double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const char* verdict = !in.md5 ? "no MD5" : (*in.md5 == digest ? "MD5 OK" : "MD5 MISMATCH");
                if (in.md5 && *in.md5 != digest) failures++;
                std::printf("  %-10s %u Hz %u ch %u bit, %llu frames, %.3f s (%.0fx) %s %s\n", decoderName(k), in.sampleRate,
                            in.channels, in.bitsPerSample, (unsigned long long)frames, s,
                            s > 0 ? framesToSeconds(frames, in.sampleRate) / s : 0.0, toHex(digest).c_str(), verdict);
            } catch (const Error& e) {
                failures++;
                std::printf("  %-10s ERROR %s: %s\n", decoderName(k), errorCodeName(e.code()), e.what());
            }
        }
    }
    return failures ? 1 : 0;
}

} // namespace camelcli
