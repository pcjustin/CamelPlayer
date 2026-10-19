// list-devices, play, play-queue, capture, gen-test-audio
#include <windows.h>
#include <conio.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Commands.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "Engine/testing/TestAudio.h"
#include "Server/Playback/LocalServer.h"
#include "Common/util/Text.h"

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

// The decoder `s` selects for `file`, by its contents.
const char* decoderLabel(const Settings& s, const std::filesystem::path& file) {
    const auto format = probeFormat(file);
    return decodersFor(format).empty() ? "none" : decoderName(decoderFor(s.decoderByFormat, format));
}

// Applies --backend/--decoder/--device/--ks-mode/--format/--buffer options on top of the saved settings.
Settings settingsFromArgs(const Args& args) {
    Settings s = loadSettings();
    if (auto v = option(args, L"--backend")) {
        if (!backendFromId(toUtf8(*v), s.backend)) throw std::runtime_error("unknown backend");
    }
    if (auto v = option(args, L"--decoder")) {
        DecoderKind decoder{};
        if (!decoderFromId(toUtf8(*v), decoder)) throw std::runtime_error("unknown decoder");
        for (const auto format : kAllFormats) // every format this decoder reads
            if (supports(format, decoder)) s.decoderByFormat[format] = decoder;
    }
    if (auto v = option(args, L"--device")) {
        // index into the backend's device list, or a raw id
        std::vector<DeviceEntry> list = s.backend == Backend::Wasapi ? listWasapiDevices()
                                        : s.backend == Backend::Asio ? listAsioDrivers()
                                                                     : listKsPins();
        std::string id = *v == L"default" ? "" : toUtf8(*v); // "default": follow the Windows default (WASAPI)
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
    if (auto v = option(args, L"--shared")) s.wasapi.shared = *v == L"1";
    if (auto v = option(args, L"--convert")) s.convertWhenUnsupported = *v == L"1";
    if (auto v = option(args, L"--dsd")) {
        const DsdOutput d = *v == L"dop" ? DsdOutput::Dop : *v == L"pcm" ? DsdOutput::Pcm : DsdOutput::Native;
        s.asio.dsdOutput = s.ks.dsdOutput = d;
        s.wasapi.dsdOutput = d == DsdOutput::Pcm ? d : DsdOutput::Dop;
    }
    if (auto v = option(args, L"--dsd-rate")) s.wasapi.dsdPcmRate = s.asio.dsdPcmRate = s.ks.dsdPcmRate = std::stoul(*v);
    if (auto v = option(args, L"--convert-dsd")) s.convertDsdWhenUnsupported = *v == L"1";
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
        std::printf("  %s  %s  underruns %llu  %s   ", describe(*s.output).c_str(), s.outputStatus.mode.c_str(),
                    (unsigned long long)s.underruns, s.outputStatus.detail.c_str());
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
        std::puts(
            "usage: play <file.flac> [--backend wasapi|asio|ks] [--decoder id] [--device n|id] [--ks-mode "
            "auto|rtevent|rtpoll|standard]\n"
            "            [--format 16|24|24in32|32] [--shared 0|1] [--dsd native|dop|pcm] [--dsd-rate 88200|176400|352800]\n"
            "            [--convert-dsd 0|1] [--seconds N] [--seek S]");
        return 2;
    }
    Settings s = settingsFromArgs(args);
    std::printf("backend %s, decoder %s\n", backendName(s.backend), decoderLabel(s, files[0]));
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

namespace {

// Counts device opens: a gapless queue opens the device once.
class CountingOutput final : public IOutput {
public:
    CountingOutput(std::unique_ptr<IOutput> inner, std::atomic<int>& opens) : inner_(std::move(inner)), opens_(opens) {
        inner_->setErrorHandler([this](const Error& e) { raiseError(e); });
        inner_->setRestartHandler([this] { raiseRestart(); });
    }
    bool supports(const DeviceFormat& f) override { return inner_->supports(f); }
    void open(const DeviceFormat& f, IRenderSource* source) override {
        ++opens_;
        inner_->open(f, source);
    }
    void start() override { inner_->start(); }
    void pause() override { inner_->pause(); }
    void close() override { inner_->close(); }
    OutputStatus status() const override { return inner_->status(); }

private:
    std::unique_ptr<IOutput> inner_;
    std::atomic<int>& opens_;
};

} // namespace

// play-queue <file>... [options]: plays the files as one Server queue and reports how often the device opened.
int cmdPlayQueue(const Args& args) {
    const auto files = positional(args);
    if (files.empty()) {
        std::puts("usage: play-queue <file.flac>... [--backend wasapi|asio|ks] [--decoder id] [--device n|id]");
        return 2;
    }
    const Settings s = settingsFromArgs(args);
    std::printf("backend %s, decoder %s\n", backendName(s.backend), decoderLabel(s, files[0]));
    std::atomic<int> opens = 0;
    server::LocalServer server(
        s, {}, [&](const Settings& settings) { return std::make_unique<CountingOutput>(createOutput(settings), opens); });
    server::Command queue;
    queue.kind = server::CommandKind::QueueReplace;
    for (const auto& file : files) queue.sources.emplace_back(toUtf8(std::filesystem::absolute(file).wstring()));
    server.submit(std::move(queue));
    server.waitIdle();
    const auto entries = server.snapshot().queue.entries;
    server::Snapshot snap;
    for (;;) {
        snap = server.snapshot();
        size_t index = 0;
        while (index < entries.size() && entries[index].id != snap.queueEntryId) ++index;
        const auto& p = snap.playback;
        std::printf("\rtrack %zu/%zu  %7.2f s  %-8s  device opens %d   ", index + 1, entries.size(),
                    p.source ? framesToSeconds(p.position, p.source->sampleRate) : 0.0,
                    p.state == server::PlaybackState::Playing  ? "playing"
                    : p.state == server::PlaybackState::Paused ? "paused"
                                                               : "stopped",
                    opens.load());
        std::fflush(stdout);
        if (p.state == server::PlaybackState::Stopped && (p.error || index + 1 == entries.size())) break;
        if (p.state == server::PlaybackState::Paused && p.error) break;
        Sleep(200);
    }
    std::puts("");
    if (snap.playback.error) std::printf("error: %s\n", snap.playback.errorDetail.c_str());
    std::printf("device opens: %d for %zu tracks\n", opens.load(), entries.size());
    return snap.playback.error ? 1 : 0;
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
                decoderLabel(s, pos[0]), "capture", sampleTypeName(type), count, ref.samples.size(),
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

// split-flac <file.flac> <parts> <dir>: cuts one track into consecutive FLAC files, for listening to gapless seams.
int cmdSplitFlac(const Args& args) {
    const auto pos = positional(args);
    if (pos.size() < 3) {
        std::puts("usage: split-flac <file.flac> <parts> <dir>");
        return 2;
    }
    const auto reference = testaudio::referenceDecode(pos[0]);
    if (!reference.error.empty()) throw std::runtime_error("cannot decode: " + reference.error);
    const size_t parts = std::stoul(pos[1]);
    const size_t channels = reference.info.channels;
    const size_t frames = reference.samples.size() / channels;
    if (!parts || parts > frames) throw std::runtime_error("invalid part count");
    const std::filesystem::path dir(pos[2]);
    const auto stem = std::filesystem::path(pos[0]).stem().wstring();
    for (size_t i = 0; i < parts; ++i) {
        const size_t first = frames * i / parts, last = frames * (i + 1) / parts;
        const std::vector<int32_t> part(reference.samples.begin() + std::ptrdiff_t(first * channels),
                                        reference.samples.begin() + std::ptrdiff_t(last * channels));
        const auto path = dir / (stem + L"-part" + std::to_wstring(i + 1) + L".flac");
        testaudio::writeFlacSamples(path, part, reference.info.sampleRate, reference.info.bitsPerSample, uint32_t(channels));
        std::printf("%s  %.2f s\n", toUtf8(path.wstring()).c_str(), double(last - first) / reference.info.sampleRate);
    }
    return 0;
}

} // namespace camelcli
