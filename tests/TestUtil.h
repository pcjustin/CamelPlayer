// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <vector>

#include "Common/AudioFormat.h"
#include "Engine/decode/IDecoder.h"
#include "Tools/testaudio/TestAudio.h"

namespace testutil {

// How many times longer an AddressSanitizer build takes, for time limits that stand for an algorithm's cost: a quadratic
// step still misses them by far.
// AirPlay's pairing, whose big-number arithmetic it slows about 25 times, has its own: for the waits a fake receiver's
// pairing has to fit in.
#if defined(__SANITIZE_ADDRESS__)
inline constexpr int kSlowdown = 4;
inline constexpr int kPairingSlowdown = 25;
#else
inline constexpr int kSlowdown = 1;
inline constexpr int kPairingSlowdown = 1;
#endif

// build\testtmp (next to build\<Configuration>\tests\tests.exe); avoids %TEMP%, which Storage Sense may wipe.
inline std::filesystem::path tempDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto dir = std::filesystem::path(exe).parent_path().parent_path().parent_path() / L"testtmp";
    std::filesystem::create_directories(dir);
    return dir;
}

// A file committed in tests/fixtures, which tests/fixtures/README.md lists.
inline std::filesystem::path fixtureFile(const std::string& name) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().parent_path().parent_path().parent_path() / L"tests" / L"fixtures" / name;
}

inline void copyFixture(const std::string& name, const std::filesystem::path& to) {
    std::filesystem::create_directories(to.parent_path());
    std::filesystem::copy_file(fixtureFile(name), to, std::filesystem::copy_options::overwrite_existing);
}

// Short versions of the §16.6 files, generated once per test run.
inline std::filesystem::path testFile(const char* name) {
    static std::once_flag once;
    std::call_once(once, [] {
        auto dir = tempDir();
        using camel::audio::testaudio::GenSpec;
        for (GenSpec s : {GenSpec{"16-44.1k", 44100, 16, 2, 2.0}, GenSpec{"24-96k", 96000, 24, 2, 2.0},
                          GenSpec{"no-md5", 48000, 16, 2, 1.0, false}})
            camel::audio::testaudio::writeFlac(dir / (s.name + ".flac"), s);
        camel::audio::testaudio::writeCorrupted(dir / "24-96k.flac", dir / "damaged.flac");
        camel::audio::testaudio::writeTruncated(dir / "24-96k.flac", dir / "truncated.flac");
        // Random bytes after the last frame, with no tag, as some rips carry.
        std::ifstream in(dir / "24-96k.flac", std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        std::mt19937 random(5);
        for (int i = 0; i < 20152; ++i) bytes.push_back(char(random() & 0xff));
        std::ofstream(dir / "trailing.flac", std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    });
    return tempDir() / (std::string(name) + ".flac");
}

// A FLAC file for `spec`, generated on first use next to the testFile set.
inline std::filesystem::path generatedFile(const camel::audio::testaudio::GenSpec& spec) {
    const auto path = tempDir() / (spec.name + ".flac");
    std::error_code error;
    if (!std::filesystem::exists(path, error)) camel::audio::testaudio::writeFlac(path, spec);
    return path;
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
inline bool knownUnsupported(camel::audio::DecoderKind k, uint32_t sampleRate) {
    return k == camel::audio::DecoderKind::Symphonia && sampleRate > 655350;
}

} // namespace testutil
