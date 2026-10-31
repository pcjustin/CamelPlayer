// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/Loudness.h"

#include <ebur128.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <future>
#include <limits>
#include <memory>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"

namespace camel::server {
namespace {

using State = std::unique_ptr<ebur128_state, void (*)(ebur128_state*)>;
using Blocks = std::array<uint64_t, 1000>;

// The energy of a histogram bin's middle and of its lower edge, as libebur128 counts them.
double binEnergy(size_t bin) {
    return std::pow(10.0, (double(bin) / 10.0 - 69.95 + 0.691) / 10.0);
}
double binFloor(size_t bin) {
    return std::pow(10.0, (double(bin) / 10.0 - 70.0 + 0.691) / 10.0);
}

// EBU R128's integrated loudness of the blocks: those 10 LU under their mean energy are left out, as in libebur128.
double gatedLoudness(const Blocks& blocks) {
    const auto mean = [&](size_t from) {
        double energy = 0.0;
        uint64_t count = 0;
        for (size_t i = from; i < blocks.size(); ++i) {
            energy += double(blocks[i]) * binEnergy(i);
            count += blocks[i];
        }
        return count == 0 ? 0.0 : energy / double(count);
    };
    const double relative = mean(0) * 0.1;
    if (relative <= 0.0) return -std::numeric_limits<double>::infinity();
    size_t from = 0;
    if (relative >= binFloor(0)) {
        from = std::min(blocks.size() - 1, size_t((10.0 * std::log10(relative) - 0.691 + 70.0) * 10.0));
        if (relative > binEnergy(from)) ++from;
    }
    const double gated = mean(from);
    return gated > 0.0 ? 10.0 * std::log10(gated) - 0.691 : -std::numeric_limits<double>::infinity();
}

double truePeak(ebur128_state* state) {
    double peak = 0.0;
    for (unsigned c = 0; c < state->channels; ++c) {
        double channel = 0.0;
        if (ebur128_true_peak(state, c, &channel) == EBUR128_SUCCESS) peak = std::max(peak, channel);
    }
    return peak;
}

// A silent file has no loudness; it is left as it is.
double gainFor(double lufs) {
    return std::isfinite(lufs) ? kReferenceLufs - lufs : 0.0;
}

struct Measured {
    Loudness loudness;
    Blocks blocks{};
};

// One file's loudness and 400 ms blocks; none when it cannot be read or `cancelled` stopped the reading.
std::optional<Measured> measure(const std::filesystem::path& file, const DecoderByFormat& decoders,
                                const std::function<bool()>& cancelled) {
    const auto format = audio::probeFormat(file);
    if (format == audio::AudioFormat::Dsd) return std::nullopt;
    const auto decoder = audio::openDecoder(decoderFor(decoders, format), file);
    const auto& info = decoder->info();
    if (info.channels == 0 || info.sampleRate == 0) return std::nullopt;
    const State state(
        ebur128_init(info.channels, info.sampleRate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_HISTOGRAM),
        [](ebur128_state* s) { ebur128_destroy(&s); });
    if (!state) return std::nullopt;
    if (info.channels == 1) ebur128_set_channel(state.get(), 0, EBUR128_DUAL_MONO);
    Measured out;
    // libebur128 ends a 400 ms block every 100 ms from 400 ms on; the frames go in up to each end, where the momentary
    // loudness is that block's.
    const size_t step = (info.sampleRate + 5) / 10;
    size_t fed = 0;
    constexpr size_t kFrames = 16384;
    std::vector<float> floats(kFrames * info.channels);
    std::vector<int32_t> integers(decoder->readsFloat() ? 0 : kFrames * info.channels);
    const float scale = 1.0f / float(1u << (std::clamp(info.bitsPerSample, 8u, 32u) - 1));
    for (;;) {
        if (cancelled && cancelled()) return std::nullopt;
        size_t frames = 0;
        if (decoder->readsFloat()) {
            frames = decoder->readFloat(floats.data(), kFrames);
        } else {
            frames = decoder->read(integers.data(), kFrames);
            for (size_t i = 0; i < frames * info.channels; ++i) floats[i] = float(integers[i]) * scale;
        }
        if (frames == 0) break;
        for (size_t at = 0; at < frames;) {
            const size_t n = std::min(frames - at, step - fed % step);
            ebur128_add_frames_float(state.get(), floats.data() + at * info.channels, n);
            at += n;
            fed += n;
            double block = 0.0;
            if (fed % step == 0 && fed >= 4 * step && ebur128_loudness_momentary(state.get(), &block) == EBUR128_SUCCESS &&
                block >= -70.0)
                ++out.blocks[std::min(out.blocks.size() - 1, size_t((block + 70.0) * 10.0))];
        }
    }
    double lufs = 0.0;
    ebur128_loudness_global(state.get(), &lufs);
    out.loudness = {gainFor(lufs), truePeak(state.get())};
    return out;
}

// The storage a path lives on: its drive, or the server and share of a network path.
std::wstring storageOf(const std::filesystem::path& path) {
    std::wstring text = path.wstring();
    for (const auto* prefix : {L"\\\\?\\UNC\\", L"\\\\?\\"}) {
        if (text.size() >= wcslen(prefix) && _wcsnicmp(text.c_str(), prefix, wcslen(prefix)) == 0) {
            text = (wcslen(prefix) == 8 ? L"\\\\" : L"") + text.substr(wcslen(prefix));
            break;
        }
    }
    size_t end = 2;
    if (text.starts_with(L"\\\\")) {
        const auto server = text.find(L'\\', 2);
        end = server == std::wstring::npos ? text.size() : std::min(text.find(L'\\', server + 1), text.size());
    }
    text.resize(std::min(end, text.size()));
    std::ranges::transform(text, text.begin(), ::towlower);
    return text;
}

constexpr const char* kNeedsAnalysis =
    "f.available=1 AND coalesce(t.format,'')<>'DSD' AND (coalesce(t.rg_track_gain,t.rg_analysis_gain) IS NULL OR "
    "(t.album_id IS NOT NULL AND coalesce(t.rg_album_gain,t.rg_analysis_album_gain) IS NULL))";

} // namespace

AlbumMeasure::AlbumMeasure(std::vector<std::filesystem::path> files)
    : files_(std::move(files)), done_(files_.size()), tracks_(files_.size()) {}

bool AlbumMeasure::run(const DecoderByFormat& decoders, unsigned threads, const std::function<bool()>& cancelled,
                       const std::function<void(size_t, const std::optional<Loudness>&)>& measured) {
    std::vector<size_t> left;
    for (size_t i = 0; i < files_.size(); ++i)
        if (!done_[i]) left.push_back(i);
    std::atomic<size_t> next = 0;
    const auto work = [&] {
        for (size_t at; (at = next++) < left.size();) {
            const size_t file = left[at];
            std::optional<Measured> result;
            try {
                result = measure(files_[file], decoders, cancelled);
            } catch (const std::exception& e) {
                log::warn("loudness: %s: %s", toUtf8(files_[file].wstring()).c_str(), e.what());
            }
            if (cancelled && cancelled()) return;
            std::lock_guard lock(mutex_);
            done_[file] = true;
            if (result) {
                tracks_[file] = result->loudness;
                for (size_t i = 0; i < blocks_.size(); ++i) blocks_[i] += result->blocks[i];
                peak_ = std::max(peak_, result->loudness.peak);
            }
            if (measured) measured(file, tracks_[file]);
        }
    };
    std::vector<std::future<void>> workers;
    for (unsigned i = 1; i < std::max(1u, threads) && i < left.size(); ++i)
        workers.push_back(std::async(std::launch::async, work));
    work();
    for (auto& w : workers) w.get();
    std::lock_guard lock(mutex_);
    return std::ranges::all_of(done_, std::identity{});
}

AlbumLoudness AlbumMeasure::result() const {
    std::lock_guard lock(mutex_);
    AlbumLoudness out{tracks_, std::nullopt};
    if (std::ranges::any_of(tracks_, [](const auto& t) { return t.has_value(); }))
        out.album = Loudness{gainFor(gatedLoudness(blocks_)), peak_};
    return out;
}

LoudnessAnalyzer::LoudnessAnalyzer(LibraryStore& store, Options options)
    : store_(store), options_(std::move(options)), thread_([this](std::stop_token stop) { run(stop); }) {}

LoudnessAnalyzer::~LoudnessAnalyzer() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

LoudnessProgress LoudnessAnalyzer::progress() const {
    return {analyzed_.load(), total_.load(), running_.load()};
}

void LoudnessAnalyzer::count() {
    auto db = store_.openReader();
    auto row = db.prepare("SELECT count(*),count(CASE WHEN coalesce(t.rg_track_gain,t.rg_analysis_gain) IS NOT NULL "
                          "THEN 1 END) FROM tracks t JOIN files f ON f.id=t.file_id "
                          "WHERE f.available=1 AND coalesce(t.format,'')<>'DSD'");
    if (row.step()) {
        total_ = row.integer(0);
        analyzed_ = row.integer(1);
    }
}

// The album of the first queued track that needs analysis, else of any such track; a track outside an album alone.
std::vector<LoudnessAnalyzer::Track> LoudnessAnalyzer::nextAlbum() {
    auto db = store_.openReader();
    std::optional<int64_t> chosen;
    auto needs =
        db.prepare(std::string("SELECT 1 FROM tracks t JOIN files f ON f.id=t.file_id WHERE t.file_id=? AND ") + kNeedsAnalysis);
    for (const auto file : options_.queued()) {
        needs.reset();
        if (!failed_.contains(file) && needs.bind(1, file).step()) {
            chosen = file;
            break;
        }
    }
    if (!chosen) {
        auto any = db.prepare(std::string("SELECT t.file_id FROM tracks t JOIN files f ON f.id=t.file_id WHERE ") +
                              kNeedsAnalysis + " ORDER BY t.album_id,t.file_id LIMIT 200");
        while (any.step())
            if (!failed_.contains(any.integer(0))) {
                chosen = any.integer(0);
                break;
            }
    }
    if (!chosen) return {};
    std::vector<Track> tracks;
    auto members = db.prepare("SELECT f.id,f.path FROM tracks t JOIN files f ON f.id=t.file_id WHERE f.available=1 AND "
                              "coalesce(t.format,'')<>'DSD' AND (t.file_id=?1 OR t.album_id=(SELECT album_id FROM tracks "
                              "WHERE file_id=?1)) ORDER BY f.id");
    members.bind(1, *chosen);
    while (members.step()) tracks.push_back({members.integer(0), std::filesystem::path(toWide(members.text(1)))});
    return tracks;
}

void LoudnessAnalyzer::run(const std::stop_token& stop) {
    using namespace std::chrono_literals;
    auto counted = std::chrono::steady_clock::time_point{};
    while (!stop.stop_requested()) {
        for (int i = 0; i < 20 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
        try {
            if (std::chrono::steady_clock::now() - counted > 10s) {
                count();
                counted = std::chrono::steady_clock::now();
            }
            const auto settings = options_.settings();
            if (settings.mode == ReplayGainMode::Off || store_.scanProgress().scans > 0) {
                running_ = false;
                continue;
            }
            const auto album = nextAlbum();
            if (album.empty()) {
                running_ = false;
                continue;
            }
            const bool throttled = settings.speed == AnalysisSpeed::Throttled;
            const auto storage = storageOf(album.front().path);
            // Throttled analysis leaves the storage to playback: it waits while the same drive or share plays.
            const auto yield = [&] {
                if (stop.stop_requested() || options_.settings().mode == ReplayGainMode::Off) return true;
                if (!throttled) return false;
                const auto playing = options_.playingSource();
                return !playing.empty() && storageOf(playing) == storage;
            };
            running_ = !yield();
            if (!running_) continue;
            std::vector<int64_t> ids;
            std::vector<std::filesystem::path> files;
            for (const auto& t : album) {
                ids.push_back(t.file);
                files.push_back(t.path);
            }
            if (!measuring_ || measuringFiles_ != ids) {
                measuring_ = std::make_unique<AlbumMeasure>(files);
                measuringFiles_ = ids;
            }
            // Each track's value is kept as it is measured; the album's once every track is.
            const unsigned threads = throttled ? 1u : std::max(1u, std::thread::hardware_concurrency());
            const bool complete =
                measuring_->run(options_.decoders(), threads, yield, [&](size_t i, const std::optional<Loudness>& track) {
                    if (!track) {
                        failed_.insert(ids[i]);
                        return;
                    }
                    store_
                        .write([&](sqlite::Database& db) {
                            db.prepare("UPDATE tracks SET rg_analysis_gain=?,rg_analysis_peak=? WHERE file_id=?")
                                .bind(1, track->gainDb)
                                .bind(2, track->peak)
                                .bind(3, ids[i])
                                .run();
                        })
                        .get();
                });
            if (!complete) continue;
            const auto result = measuring_->result();
            measuring_.reset();
            if (result.album)
                store_
                    .write([&](sqlite::Database& db) {
                        auto update =
                            db.prepare("UPDATE tracks SET rg_analysis_album_gain=?,rg_analysis_album_peak=? WHERE file_id=?");
                        for (size_t i = 0; i < ids.size(); ++i) {
                            if (!result.tracks[i]) continue;
                            update.reset();
                            update.bind(1, result.album->gainDb).bind(2, result.album->peak).bind(3, ids[i]).run();
                        }
                    })
                    .get();
            count();
            counted = std::chrono::steady_clock::now();
        } catch (const std::exception& e) {
            log::warn("loudness: %s", e.what());
        }
    }
}

} // namespace camel::server
