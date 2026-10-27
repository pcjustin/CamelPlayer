// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// camelcli: command-line companion of CamelPlayer (docs/Plan.md §4, §16).
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "Commands.h"
#include "Common/Version.h"

namespace {

void usage() {
    std::puts(
        "camelcli <command> [options]\n"
        "  --version\n"
        "      print the CamelPlayer version\n"
        "  decode <file> [--decoder ffmpeg|symphonia|drflac|sndfile|drwav|all]\n"
        "      decode the whole file (FLAC, ALAC, WAV, AIFF); FLAC is compared with its STREAMINFO MD5\n"
        "  list-devices\n"
        "      list WASAPI endpoints, ASIO drivers and Kernel Streaming pins\n"
        "  play <file.flac> [--backend wasapi|asio|ks] [--decoder id] [--device n|id] [--ks-mode auto|rtevent|rtpoll|standard]\n"
        "       [--wasapi-poll 1] [--format 16|24|24in32|32] [--convert 1] [--seconds N] [--seek S]\n"
        "      play through a real device (space = pause, s = +10 s, q = quit)\n"
        "  play-queue <file.flac>... [--backend wasapi|asio|ks] [--decoder id] [--device n|id]\n"
        "      play the files as one queue (gapless where formats match) and report device opens\n"
        "  capture <file.flac> <out.raw> [--decoder id] [--accept 16,24,24in32,32]\n"
        "      run the engine into a file and compare it bit for bit with the libFLAC reference\n"
        "  gen-test-audio [dir] [--seconds N]\n"
        "      write the FLAC test matrix (docs/Plan.md §16.6), default build\\testaudio\n"
        "  gen-library <dir> [--tracks N] [--per-album N]\n"
        "      write a synthetic tagged library with covers for UI measurements (docs/Plan.md §12.1)\n"
        "  split-flac <file.flac> <parts> <dir>\n"
        "      cut one track into consecutive FLAC files, to listen to gapless seams with play-queue\n"
        "  bench-decoders <dir|files...> [--runs N] [--out results.json]\n"
        "      measure the four decoders (docs/Plan.md §16.5)\n"
        "  scan-bench <folder> [--data <dir>]\n"
        "      build a fresh library of a folder as the player does (scan, fingerprints, covers, search text) and time it\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }
    std::wstring cmd = args[0];
    args.erase(args.begin());
    if (cmd == L"--version") {
        std::puts("camelcli " CAMEL_VERSION_STRING);
        return 0;
    }
    try {
        if (cmd == L"decode") return camelcli::cmdDecode(args);
        if (cmd == L"list-devices") return camelcli::cmdListDevices(args);
        if (cmd == L"play") return camelcli::cmdPlay(args);
        if (cmd == L"play-queue") return camelcli::cmdPlayQueue(args);
        if (cmd == L"capture") return camelcli::cmdCapture(args);
        if (cmd == L"gen-test-audio") return camelcli::cmdGenTestAudio(args);
        if (cmd == L"split-flac") return camelcli::cmdSplitFlac(args);
        if (cmd == L"gen-library") return camelcli::cmdGenLibrary(args);
        if (cmd == L"bench-one") return camelcli::cmdBenchOne(args);
        if (cmd == L"bench-decoders") return camelcli::cmdBenchDecoders(args);
        if (cmd == L"scan-bench") return camelcli::cmdScanBench(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
