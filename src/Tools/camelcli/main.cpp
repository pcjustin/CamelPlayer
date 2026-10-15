// camelcli: command-line companion of CamelPlayer (docs/計劃書.md §4, §16).
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "Commands.h"

namespace {

void usage() {
    std::puts(
        "camelcli <command> [options]\n"
        "  decode <file.flac> [--decoder ffmpeg|symphonia|drflac|sndfile|all]\n"
        "      decode the whole file and compare the result with the STREAMINFO MD5\n"
        "  list-devices\n"
        "      list WASAPI endpoints, ASIO drivers and Kernel Streaming pins\n"
        "  play <file.flac> [--backend wasapi|asio|ks] [--decoder id] [--device n|id] [--ks-mode auto|rtevent|rtpoll|standard]\n"
        "       [--wasapi-poll 1] [--format 16|24|24in32|32] [--seconds N] [--seek S]\n"
        "      play through a real device (space = pause, s = +10 s, q = quit)\n"
        "  capture <file.flac> <out.raw> [--decoder id] [--accept 16,24,24in32,32]\n"
        "      run the engine into a file and compare it bit for bit with the libFLAC reference\n"
        "  gen-test-audio [dir] [--seconds N]\n"
        "      write the FLAC test matrix (docs/計劃書.md §16.6), default build\\testaudio\n"
        "  bench-decoders <dir|files...> [--runs N] [--out results.json]\n"
        "      measure the four decoders (docs/計劃書.md §16.5)\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }
    std::wstring cmd = args[0];
    args.erase(args.begin());
    try {
        if (cmd == L"decode") return camelcli::cmdDecode(args);
        if (cmd == L"list-devices") return camelcli::cmdListDevices(args);
        if (cmd == L"play") return camelcli::cmdPlay(args);
        if (cmd == L"capture") return camelcli::cmdCapture(args);
        if (cmd == L"gen-test-audio") return camelcli::cmdGenTestAudio(args);
        if (cmd == L"bench-one") return camelcli::cmdBenchOne(args);
        if (cmd == L"bench-decoders") return camelcli::cmdBenchDecoders(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
