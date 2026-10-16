// list-devices, play, capture, gen-test-audio
#include <windows.h>
#include <conio.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Commands.h"
#include "audio/engine/PlaybackEngine.h"
#include "audio/engine/SamplePacker.h"
#include "audio/output/CaptureOutput.h"
#include "audio/testing/TestAudio.h"
#include "util/Text.h"

using namespace camel;
using namespace camel::audio;

namespace camelcli {

namespace {

void printList(const char* title, const std::vector<DeviceEntry>& list) {
    std::printf("%s\n", title);
    if (list.empty()) std::puts("  (none)");
    for (size_t i = 0; i < list.size(); i++)
        std::printf("  [%zu] %s\n      id: %s\n", i, list[i].name.c_str(), list[i].id.c_str());
}

// Applies --backend/--decoder/--device/--ks-mode/--format/--buffer options on top of the saved settings.
Settings settingsFromArgs(const Args& args) {
    Settings s = loadSettings();
    if (auto v = option(args, L"--backend")) {
        if (!backendFromId(toUtf8(*v), s.backend)) throw std::runtime_error("unknown backend");
    }
    if (auto v = option(args, L"--decoder")) {
        if (!decoderFromId(toUtf8(*v), s.decoder)) throw std::runtime_error("unknown decoder");
    }
    if (auto v = option(args, L"--device")) {
        // index into the backend's device list, or a raw id
        std::vector<DeviceEntry> list = s.backend == Backend::Wasapi ? listWasapiDevices()
                                        : s.backend == Backend::Asio ? listAsioDrivers()
                                                                     : listKsPins();
        std::string id = toUtf8(*v);
        if (!id.empty() && std::all_of(id.begin(), id.end(), ::isdigit) && std::stoul(id) < list.size())
            id = list[std::stoul(id)].id;
        if (s.backend == Backend::Wasapi)
            s.wasapi.deviceId = id;
        else if (s.backend == Backend::Asio)
            s.asio.driverClsid = id;
        else
            s.ks.deviceId = id;
    }
    if (auto v = option(args, L"--ks-mode")) {
        std::string m = toUtf8(*v);
        s.ks.mode = m == "rtevent"    ? KsMode::WaveRtEvent
                    : m == "rtpoll"   ? KsMode::WaveRtPoll
                    : m == "standard" ? KsMode::Standard
                                      : KsMode::Auto;
    }
    if (auto v = option(args, L"--wasapi-poll")) s.wasapi.eventDriven = *v != L"1";
    if (auto v = option(args, L"--format")) {
        auto t = sampleTypeFromId(toUtf8(*v));
        s.wasapi.sampleType = t;
        s.ks.sampleType = t;
    }
    return s;
}

const char* stateName(PlayState s) {
    return s == PlayState::Playing ? "playing" : s == PlayState::Paused ? "paused" : "stopped";
}

void printSnapshot(const EngineSnapshot& s) {
    std::printf("\r%-8s %7.2f / %7.2f s", stateName(s.state), s.source ? framesToSeconds(s.position, s.source->sampleRate) : 0.0,
                s.source ? framesToSeconds(s.total, s.source->sampleRate) : 0.0);
    if (s.output)
        std::printf("  %s  %s  underruns %llu   ", describe(*s.output).c_str(), s.outputStatus.mode.c_str(),
                    (unsigned long long)s.underruns);
    if (s.error) std::printf("  [%s] %s", errorCodeName(*s.error), s.errorDetail.c_str());
    std::fflush(stdout);
}

} // namespace

int cmdListDevices(const Args&) {
    printList("WASAPI endpoints:", listWasapiDevices());
    printList("ASIO drivers:", listAsioDrivers());
    printList("Kernel Streaming pins:", listKsPins());
    return 0;
}

// play <file> [options] [--seconds N] [--seek S]: keys: space = pause/resume, s = +10 s, q = quit
int cmdPlay(const Args& args) {
    auto files = positional(args);
    if (files.empty()) {
        std::puts("usage: play <file.flac> [--backend wasapi|asio|ks] [--decoder id] [--device n|id] [--ks-mode "
                  "auto|rtevent|rtpoll|standard]\n"
                  "            [--format 16|24|24in32|32] [--seconds N] [--seek S]");
        return 2;
    }
    Settings s = settingsFromArgs(args);
    std::printf("backend %s, decoder %s\n", backendName(s.backend), decoderName(s.decoder));
    PlaybackEngine engine(s);
    engine.openAndPlay(files[0]);
    engine.waitIdle();
    if (auto v = option(args, L"--seek")) {
        auto snap = engine.snapshot();
        if (snap.source) engine.seek(secondsToFrames(std::stod(*v), snap.source->sampleRate));
    }
    double limit = option(args, L"--seconds") ? std::stod(*option(args, L"--seconds")) : 1e9;
    auto t0 = GetTickCount64();
    EngineSnapshot snap;
    for (;;) {
        engine.waitIdle();
        snap = engine.snapshot();
        printSnapshot(snap);
        if (snap.state == PlayState::Stopped || (snap.state == PlayState::Paused && snap.error)) break;
        if ((GetTickCount64() - t0) / 1000.0 >= limit) break;
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q') break;
            if (c == ' ') engine.togglePause();
            if (c == 's' && snap.source) engine.seek(snap.position + 10ull * snap.source->sampleRate);
        }
        Sleep(200);
    }
    engine.stop();
    engine.waitIdle();
    std::puts("");
    if (snap.outputStatus.detail.size()) std::printf("output: %s\n", snap.outputStatus.detail.c_str());
    return snap.error ? 1 : 0;
}

// capture <file> <out.raw> [--decoder id] [--accept 16,24,24in32,32]: runs the engine into CaptureOutput and
// compares the captured bytes with the libFLAC reference decode.
int cmdCapture(const Args& args) {
    auto pos = positional(args);
    if (pos.size() < 2) {
        std::puts("usage: capture <file.flac> <out.raw> [--decoder id] [--accept 16,24,24in32,32]");
        return 2;
    }
    Settings s = settingsFromArgs(args);
    std::vector<SampleType> accept;
    if (auto v = option(args, L"--accept")) {
        std::string list = toUtf8(*v);
        size_t at = 0;
        while (at <= list.size()) {
            size_t comma = list.find(',', at);
            if (auto t = sampleTypeFromId(list.substr(at, comma - at))) accept.push_back(*t);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
    }
    CaptureOptions co;
    co.file = pos[1];
    if (!accept.empty())
        co.accepts = [accept](const DeviceFormat& f) { return std::find(accept.begin(), accept.end(), f.type) != accept.end(); };
    PlaybackEngine engine(s, [&](const Settings&) { return createCaptureOutput(co); });
    engine.openAndPlay(pos[0]);
    engine.waitIdle();
    engine.waitForState(PlayState::Stopped, std::chrono::minutes(10));
    auto snap = engine.snapshot();
    if (snap.error) {
        std::printf("engine error: [%s] %s\n", errorCodeName(*snap.error), snap.errorDetail.c_str());
        return 1;
    }
    // The format the engine picked is not in the snapshot after the stream closed; recompute it.
    auto ref = testaudio::referenceDecode(pos[0]);
    if (!ref.error.empty()) {
        std::printf("reference decode: %s\n", ref.error.c_str());
        return 1;
    }
    if (ref.info.md5 && !ref.md5Ok) {
        std::puts("reference decode: STREAMINFO MD5 mismatch");
        return 1;
    }
    std::ifstream in(pos[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    SampleType type = SampleType::Int32;
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32}) {
        if (!canCarryBitPerfect(t, ref.info.bitsPerSample)) continue;
        if (!accept.empty() && std::find(accept.begin(), accept.end(), t) == accept.end()) continue;
        type = t;
        break;
    }
    const size_t count = bytes.size() / bytesPerSample(type);
    std::vector<int32_t> got(count);
    unpackSamples(bytes.data(), count, ref.info.bitsPerSample, type, got.data());
    bool same = got == ref.samples;
    std::printf("%s: %s via %s as %s, %zu samples captured, reference %zu -> %s\n", toUtf8(pos[0]).c_str(),
                decoderName(s.decoder), "capture", sampleTypeName(type), count, ref.samples.size(),
                same ? "BIT-IDENTICAL" : "DIFFERENT");
    return same ? 0 : 1;
}

int cmdGenTestAudio(const Args& args) {
    if (auto db = option(args, L"--tone")) { // gen-test-audio <dir> --tone -40 --rate 96000 --bits 24
        auto pos = positional(args);
        std::filesystem::path dir = pos.empty() ? std::filesystem::path(L"build\\testaudio") : std::filesystem::path(pos[0]);
        testaudio::GenSpec s{"", 96000, 24, 2, 10.0};
        s.toneDb = std::stod(*db);
        if (auto v = option(args, L"--rate")) s.sampleRate = uint32_t(std::stoul(*v));
        if (auto v = option(args, L"--bits")) s.bits = uint32_t(std::stoul(*v));
        if (auto v = option(args, L"--seconds")) s.seconds = std::stod(*v);
        s.name = "tone" + std::to_string(int(s.toneDb)) + "dB-" + std::to_string(s.bits) + "-" + std::to_string(s.sampleRate);
        auto p = dir / (s.name + ".flac");
        testaudio::writeFlac(p, s);
        std::printf("%s\n", toUtf8(p.wstring()).c_str());
        return 0;
    }
    auto pos = positional(args);
    std::filesystem::path dir = pos.empty() ? std::filesystem::path(L"build\\testaudio") : std::filesystem::path(pos[0]);
    double seconds = option(args, L"--seconds") ? std::stod(*option(args, L"--seconds")) : 10.0;
    for (auto& p : testaudio::generateAll(dir, seconds))
        std::printf("%s  %llu bytes\n", toUtf8(p.wstring()).c_str(), (unsigned long long)std::filesystem::file_size(p));
    return 0;
}

} // namespace camelcli
