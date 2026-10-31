// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/ImpulseResponse.h"

#include <sndfile.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>

#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Common/util/Zip.h"

namespace camel::audio {
namespace {

constexpr uint32_t kCacheMagic = 0x52495043; // "CPIR"
constexpr size_t kMemoryEntries = 4;

// libsndfile reading a .wav held in memory.
class MemoryFile {
public:
    explicit MemoryFile(const std::string& data) : data_(data) {}

    SNDFILE* open(SF_INFO& info) {
        static SF_VIRTUAL_IO io = [] {
            SF_VIRTUAL_IO callbacks{};
            callbacks.get_filelen = [](void* self) { return sf_count_t(static_cast<MemoryFile*>(self)->data_.size()); };
            callbacks.seek = [](sf_count_t offset, int whence, void* self) {
                auto& file = *static_cast<MemoryFile*>(self);
                const sf_count_t base = whence == SEEK_CUR ? file.at_ : whence == SEEK_END ? sf_count_t(file.data_.size()) : 0;
                file.at_ = std::clamp<sf_count_t>(base + offset, 0, sf_count_t(file.data_.size()));
                return file.at_;
            };
            callbacks.read = [](void* dst, sf_count_t count, void* self) {
                auto& file = *static_cast<MemoryFile*>(self);
                const sf_count_t n = std::clamp<sf_count_t>(sf_count_t(file.data_.size()) - file.at_, 0, count);
                std::memcpy(dst, file.data_.data() + file.at_, size_t(n));
                file.at_ += n;
                return n;
            };
            callbacks.write = [](const void*, sf_count_t, void*) { return sf_count_t(0); };
            callbacks.tell = [](void* self) { return static_cast<MemoryFile*>(self)->at_; };
            return callbacks;
        }();
        info = {};
        return sf_open_virtual(&io, SFM_READ, &info, this);
    }

private:
    const std::string& data_;
    sf_count_t at_ = 0;
};

struct Wav {
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0;
    std::vector<double> samples; // interleaved
};

// The .wav in `data`: its format, and its samples when `withSamples`.
std::optional<Wav> readWav(const std::string& data, bool withSamples) {
    MemoryFile file(data);
    SF_INFO info;
    SNDFILE* handle = file.open(info);
    if (!handle) return std::nullopt;
    Wav wav{uint32_t(info.samplerate), uint32_t(info.channels), uint64_t(info.frames), {}};
    if (withSamples) {
        wav.samples.resize(size_t(info.frames) * size_t(info.channels));
        const sf_count_t read = sf_readf_double(handle, wav.samples.data(), info.frames);
        wav.frames = uint64_t(std::max<sf_count_t>(read, 0));
        wav.samples.resize(size_t(wav.frames) * wav.channels);
    }
    sf_close(handle);
    if (wav.rate == 0 || wav.frames == 0) return std::nullopt;
    return wav;
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ImpulseError(ImpulseProblem::Unreadable, "cannot open " + toUtf8(file.wstring()));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint64_t fnv1a(const std::string& data) {
    uint64_t hash = 0xcbf29ce484222325;
    for (const char c : data) hash = (hash ^ uint8_t(c)) * 0x100000001b3;
    return hash;
}

// The candidate closest to `rate` on a log scale; on a tie the higher one, which loses less when converted down.
uint32_t nearestRate(const std::vector<uint32_t>& rates, uint32_t rate) {
    return *std::ranges::min_element(rates, [&](uint32_t a, uint32_t b) {
        const double da = std::abs(std::log(double(a) / rate)), db = std::abs(std::log(double(b) / rate));
        return da != db ? da < db : a > b;
    });
}

std::filesystem::path cacheFile(uint64_t hash, uint32_t rate) {
    char name[64];
    snprintf(name, sizeof name, "%016llx-%u.ir", static_cast<unsigned long long>(hash), rate);
    return dataDirectory() / L"ir-cache" / name;
}

std::optional<Wav> readCache(const std::filesystem::path& file, uint32_t rate) {
    std::ifstream in(file, std::ios::binary);
    uint32_t header[4] = {};
    uint64_t frames = 0;
    if (!in.read(reinterpret_cast<char*>(header), sizeof header) || !in.read(reinterpret_cast<char*>(&frames), sizeof frames) ||
        header[0] != kCacheMagic || header[1] != rate || !(header[2] == 1 || header[2] == 2 || header[2] == 4) || frames == 0 ||
        frames > kMaxImpulseTaps)
        return std::nullopt;
    Wav wav{rate, header[2], frames, std::vector<double>(size_t(frames) * header[2])};
    if (!in.read(reinterpret_cast<char*>(wav.samples.data()), std::streamsize(wav.samples.size() * sizeof(double))))
        return std::nullopt;
    return wav;
}

void writeCache(const std::filesystem::path& file, const Wav& wav, uint32_t sourceRate) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const uint32_t header[4] = {kCacheMagic, wav.rate, wav.channels, sourceRate};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        out.write(reinterpret_cast<const char*>(&wav.frames), sizeof wav.frames);
        out.write(reinterpret_cast<const char*>(wav.samples.data()), std::streamsize(wav.samples.size() * sizeof(double)));
        if (!out) {
            log::warn("impulse response: cannot cache %s", toUtf8(file.wstring()).c_str());
            return;
        }
    }
    std::filesystem::rename(temporary, file, error);
}

// `wav` at `rate` through soxr. Silence around the response gives the resampler's ringing room before the first tap
// and after the last, and is trimmed back to where the result reaches -140 dB of its peak, so a response that starts
// at once keeps its level. An impulse response keeps its gain, so its taps scale with the rate change.
Wav resample(const Wav& wav, uint32_t rate) {
    constexpr size_t kPad = 4096;
    const double ratio = double(rate) / wav.rate;
    std::vector<double> padded((size_t(wav.frames) + 2 * kPad) * wav.channels, 0.0);
    std::ranges::copy(wav.samples, padded.begin() + std::ptrdiff_t(kPad * wav.channels));
    const size_t inputFrames = padded.size() / wav.channels;
    const size_t capacity = size_t(std::ceil(double(inputFrames) * ratio)) + 1;
    std::vector<double> out(capacity * wav.channels);
    const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_VHQ, 0);
    size_t done = 0;
    if (const soxr_error_t error = soxr_oneshot(wav.rate, rate, wav.channels, padded.data(), inputFrames, nullptr, out.data(),
                                                capacity, &done, &io, &quality, nullptr))
        throw ImpulseError(ImpulseProblem::Unreadable, std::string("cannot convert the rate: ") + error);
    out.resize(done * wav.channels);
    double peak = 0.0;
    for (const double v : out) peak = std::max(peak, std::abs(v));
    const auto audible = [&](double v) { return std::abs(v) >= peak * 1e-7; };
    const auto first = std::ranges::find_if(out, audible) - out.begin();
    const auto last = out.rend() - std::ranges::find_if(out.rbegin(), out.rend(), audible);
    const size_t begin = size_t(first) / wav.channels, end = (size_t(last) + wav.channels - 1) / wav.channels;
    Wav result{rate, wav.channels, end > begin ? end - begin : 0, {}};
    result.samples.assign(out.begin() + std::ptrdiff_t(begin * wav.channels), out.begin() + std::ptrdiff_t(end * wav.channels));
    for (auto& v : result.samples) v /= ratio;
    if (result.frames == 0) throw ImpulseError(ImpulseProblem::Unreadable, "the response is silent");
    return result;
}

Impulse prepare(const Wav& wav) {
    const CamelImpulseResponseV1 abi{
        sizeof(CamelImpulseResponseV1), CAMEL_ENGINE_ABI_VERSION, wav.channels, wav.rate, wav.frames, wav.samples.data()};
    CamelImpulse* handle = nullptr;
    if (camel_impulse_create_v1(&abi, &handle) != CAMEL_ENGINE_OK || !handle)
        throw ImpulseError(ImpulseProblem::Unreadable, "the response holds values that cannot be used");
    return Impulse(handle, [](CamelImpulse* p) { camel_impulse_destroy_v1(p); });
}

LoadedImpulse load(const std::filesystem::path& file, uint32_t rateHz) {
    const std::string data = readFile(file);
    // The .wav files the source holds, by rate.
    std::vector<std::pair<uint32_t, const std::string*>> sources;
    std::vector<ZipEntry> entries;
    auto extension = file.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    if (extension == L".zip") {
        try {
            entries = readZip(data);
        } catch (const std::exception& e) {
            throw ImpulseError(ImpulseProblem::Unreadable, e.what());
        }
        for (const auto& entry : entries) {
            auto name = entry.name;
            std::ranges::transform(name, name.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
            if (entry.directory || !name.ends_with(".wav") || name.find("__macosx") != std::string::npos) continue;
            if (const auto header = readWav(entry.data, false)) sources.emplace_back(header->rate, &entry.data);
        }
    } else if (const auto header = readWav(data, false)) {
        sources.emplace_back(header->rate, &data);
    }
    if (sources.empty()) throw ImpulseError(ImpulseProblem::Unreadable, "no .wav impulse response in the file");
    LoadedImpulse loaded;
    for (const auto& [rate, _] : sources)
        if (std::ranges::find(loaded.info.rates, rate) == loaded.info.rates.end()) loaded.info.rates.push_back(rate);
    std::ranges::sort(loaded.info.rates);
    const uint32_t sourceRate = nearestRate(loaded.info.rates, rateHz);
    const auto chosen = std::ranges::find(sources, sourceRate, &std::pair<uint32_t, const std::string*>::first)->second;
    auto wav = readWav(*chosen, true);
    if (!wav) throw ImpulseError(ImpulseProblem::Unreadable, "the .wav cannot be read");
    if (wav->channels != 1 && wav->channels != 2 && wav->channels != 4)
        throw ImpulseError(ImpulseProblem::Layout, std::to_string(wav->channels) + " channels");
    const auto atRate = uint64_t(std::ceil(double(wav->frames) * rateHz / sourceRate));
    if (atRate > kMaxImpulseTaps) throw ImpulseError(ImpulseProblem::TooLong, std::to_string(atRate) + " frames");
    loaded.info.sourceRate = sourceRate;
    loaded.info.channels = wav->channels;
    if (sourceRate != rateHz) {
        loaded.info.converted = true;
        const auto cached = cacheFile(fnv1a(*chosen), rateHz);
        if (auto hit = readCache(cached, rateHz); hit && hit->channels == wav->channels) {
            wav = std::move(hit);
        } else {
            wav = resample(*wav, rateHz);
            if (wav->frames > kMaxImpulseTaps)
                throw ImpulseError(ImpulseProblem::TooLong, std::to_string(wav->frames) + " frames");
            writeCache(cached, *wav, sourceRate);
        }
    }
    loaded.info.frames = wav->frames;
    loaded.impulse = prepare(*wav);
    return loaded;
}

struct MemoryEntry {
    std::filesystem::path file;
    std::filesystem::file_time_type written;
    uintmax_t size = 0;
    uint32_t rate = 0;
    LoadedImpulse loaded;
};

} // namespace

LoadedImpulse loadImpulse(const std::filesystem::path& file, uint32_t rateHz) {
    static std::mutex mutex;
    static std::list<MemoryEntry> recent;
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    const auto size = std::filesystem::file_size(file, error);
    if (error) throw ImpulseError(ImpulseProblem::Unreadable, "cannot open " + toUtf8(file.wstring()));
    {
        std::lock_guard lock(mutex);
        const auto hit = std::ranges::find_if(recent, [&](const MemoryEntry& e) {
            return e.file == file && e.written == written && e.size == size && e.rate == rateHz;
        });
        if (hit != recent.end()) {
            recent.splice(recent.begin(), recent, hit);
            return hit->loaded;
        }
    }
    auto loaded = load(file, rateHz);
    std::lock_guard lock(mutex);
    recent.push_front({file, written, size, rateHz, loaded});
    if (recent.size() > kMemoryEntries) recent.pop_back();
    return loaded;
}

} // namespace camel::audio
