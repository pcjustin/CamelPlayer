#pragma once
#include <windows.h>

#include <filesystem>
#include <mutex>

#include "audio/decode/IDecoder.h"
#include "audio/testing/TestAudio.h"

namespace testutil {

// build\testtmp (next to build\<Configuration>\tests\tests.exe); avoids %TEMP%, which Storage Sense may wipe.
inline std::filesystem::path tempDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto dir = std::filesystem::path(exe).parent_path().parent_path().parent_path() / L"testtmp";
    std::filesystem::create_directories(dir);
    return dir;
}

// Short versions of the §16.6 files, generated once per test run.
inline std::filesystem::path testFile(const char* name) {
    static std::once_flag once;
    std::call_once(once, [] {
        auto dir = tempDir();
        using camel::audio::testaudio::GenSpec;
        for (GenSpec s : {GenSpec{"16-44.1k", 44100, 16, 2, 2.0}, GenSpec{"24-96k", 96000, 24, 2, 2.0},
                          GenSpec{"32-384k", 384000, 32, 2, 1.0}, GenSpec{"20-48k", 48000, 20, 2, 1.0},
                          GenSpec{"no-md5", 48000, 16, 2, 1.0, false}})
            camel::audio::testaudio::writeFlac(dir / (s.name + ".flac"), s);
        camel::audio::testaudio::writeCorrupted(dir / "24-96k.flac", dir / "damaged.flac");
        camel::audio::testaudio::writeTruncated(dir / "24-96k.flac", dir / "truncated.flac");
    });
    return tempDir() / (std::string(name) + ".flac");
}

inline std::filesystem::path acceptanceFile(const char* name) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = tempDir() / "acceptance" / std::filesystem::path(exe).parent_path().parent_path().filename();
    static std::once_flag once;
    std::call_once(once, [&] { camel::audio::testaudio::generateAll(dir); });
    return dir / (std::string(name) + ".flac");
}

// Formats a decoder is known not to handle (findings recorded in docs/Decoder-comparison.md). Opening such a file must
// throw camel::audio::Error instead of crashing or producing wrong samples.
inline bool knownUnsupported(camel::audio::DecoderKind k, uint32_t bits, uint32_t sampleRate) {
    using camel::audio::DecoderKind;
    switch (k) {
    case DecoderKind::FFmpeg:
        return false;
    case DecoderKind::Symphonia:
        return bits == 32 || sampleRate > 655350;
    case DecoderKind::DrFlac:
        return bits == 32;
    case DecoderKind::Sndfile:
        return bits != 8 && bits != 16 && bits != 24;
    }
    return false;
}

} // namespace testutil
