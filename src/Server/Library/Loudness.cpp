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
#include "Server/Library/Waveform.h"

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
    std::vector<uint8_t> peaks; // with `waveform`
};

// One file's loudness and 400 ms blocks, and with `waveform` its waveform from the same decoding; none when it cannot be
// read or `cancelled` stopped the reading.
std::optional<Measured> measure(const std::filesystem::path& file, const DecoderByFormat& decoders,
                                const std::function<bool()>& cancelled, bool waveform) {
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
    std::optional<PeakCollector> peaks;
    if (waveform) peaks.emplace(info.sampleRate, info.channels);
    // libebur128 ends a 400 ms block every 100 ms from 400 ms on; the frames go in up to each end, where the momentary
    // loudness is that block's.
    const size_t step = (info.sampleRate + 5) / 10;
    size_t fed = 0;
    constexpr size_t kFrames = 16384;
    std::vector<float> floats(kFrames * info.channels);
    std::vector<int32_t> integers(decoder->readsFloat() ? 0 : kFrames * info.channels);
    const float scale = 1.0f / float(1u << (std::clamp(info.bitsPerSample, 8u, 32u) - 1));
    uint64_t decoded = 0;
    for (;;) {
        if (cancelled && cancelled()) return std::nullopt;
        size_t frames = 0;
        try {
            if (decoder->readsFloat()) {
                frames = decoder->readFloat(floats.data(), kFrames);
            } else {
                frames = decoder->read(integers.data(), kFrames);
                for (size_t i = 0; i < frames * info.channels; ++i) floats[i] = float(integers[i]) * scale;
            }
        } catch (const audio::Error&) {
            // A FLAC whose length or MD5 disagrees with its STREAMINFO fails only at its end, as playback reports;
            // what was decoded before stands.
            if (decoded == 0) throw;
            break;
        }
        if (frames == 0) break;
        decoded += frames;
        if (peaks) peaks->add(floats.data(), frames);
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
    if (peaks) out.peaks = peaks->peaks();
    return out;
}

// DSD is not analyzed; tracks.format holds formatId().
std::string notDsd() {
    return std::string("coalesce(t.format,'')<>'") + audio::formatId(audio::AudioFormat::Dsd) + "'";
}

// Files of a root that is offline are not tried.
const char* rootOnline() {
    return "EXISTS(SELECT 1 FROM roots r WHERE r.id=f.root_id AND r.online=1)";
}

// " AND `column` NOT IN (...)" for the files left out, or nothing; left out before a query's limit, so files that failed
// never fill it.
std::string leftOut(const char* column, const std::set<int64_t>& failed, const std::map<int64_t, int64_t>& away) {
    std::string ids;
    for (const auto id : failed) ids += (ids.empty() ? "" : ",") + std::to_string(id);
    for (const auto& [id, root] : away) ids += (ids.empty() ? "" : ",") + std::to_string(id);
    return ids.empty() ? std::string() : std::string(" AND ") + column + " NOT IN (" + ids + ")";
}

std::string needsAnalysis() {
    return "f.available=1 AND " + std::string(rootOnline()) + " AND " + notDsd() +
           " AND (coalesce(t.rg_track_gain,t.rg_analysis_gain) IS NULL OR "
           "(t.album_id IS NOT NULL AND coalesce(t.rg_album_gain,t.rg_analysis_album_gain) IS NULL))";
}

} // namespace

AlbumMeasure::AlbumMeasure(std::vector<std::filesystem::path> files, bool waveforms)
    : files_(std::move(files)), waveforms_(waveforms), peaks_(files_.size()), done_(files_.size()), tracks_(files_.size()) {}

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
            // What stopped the reading, not what `cancelled` says afterwards: a file left for a moment is measured
            // again, and only one that failed is recorded without a value.
            bool interrupted = false;
            const std::function<bool()> stop = [&] { return interrupted = cancelled && cancelled(); };
            try {
                result = measure(files_[file], decoders, stop, waveforms_);
            } catch (const std::exception& e) {
                log::warn("loudness: %s: %s", toUtf8(files_[file].wstring()).c_str(), e.what());
            }
            if (interrupted) return;
            std::optional<Loudness> track;
            {
                std::lock_guard lock(mutex_);
                done_[file] = true;
                if (result) {
                    tracks_[file] = result->loudness;
                    peaks_[file] = std::move(result->peaks);
                    for (size_t i = 0; i < blocks_.size(); ++i) blocks_[i] += result->blocks[i];
                    peak_ = std::max(peak_, result->loudness.peak);
                }
                track = tracks_[file];
            }
            // One file at a time, and outside the lock, so the callback may read this file's waveform.
            if (measured) {
                std::lock_guard serial(measuredMutex_);
                measured(file, track);
            }
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

std::vector<uint8_t> AlbumMeasure::waveform(size_t file) const {
    std::lock_guard lock(mutex_);
    return file < peaks_.size() ? peaks_[file] : std::vector<uint8_t>{};
}

LoudnessAnalyzer::LoudnessAnalyzer(LibraryStore& store, Options options)
    : store_(store), options_(std::move(options)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

LoudnessAnalyzer::~LoudnessAnalyzer() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

LoudnessProgress LoudnessAnalyzer::progress() const {
    progressAsked_ = std::chrono::steady_clock::now().time_since_epoch().count();
    return {analyzed_.load(), total_.load(), running_.load()};
}

void LoudnessAnalyzer::count() {
    auto db = store_.openReader();
    auto row = db.prepare("SELECT count(*),count(CASE WHEN coalesce(t.rg_track_gain,t.rg_analysis_gain) IS NOT NULL "
                          "THEN 1 END) FROM tracks t JOIN files f ON f.id=t.file_id "
                          "WHERE f.available=1 AND " +
                          notDsd());
    if (row.step()) {
        total_ = row.integer(0);
        analyzed_ = row.integer(1);
    }
}

std::optional<std::vector<uint8_t>> LoudnessAnalyzer::playingWaveform(const std::filesystem::path& path) const {
    std::lock_guard lock(playingMutex_);
    const auto found = playingWaveforms_.find(path);
    return found == playingWaveforms_.end() ? std::nullopt : std::optional(found->second);
}

int LoudnessAnalyzer::minuteOfDay() const {
    if (options_.minuteOfDay) return options_.minuteOfDay();
    const auto local = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    return int(std::chrono::duration_cast<std::chrono::minutes>(local - std::chrono::floor<std::chrono::days>(local)).count());
}

bool LoudnessAnalyzer::waveformsOn() const {
    return options_.waveform && options_.waveform().enabled;
}

// A decoding's check between chunks: it waits while playback needs the file's storage (docs/Plan.md §7.7), and stops
// for good when the analysis ends or waveforms are turned off.
std::function<bool()> LoudnessAnalyzer::givingWay(const std::stop_token& stop, const std::filesystem::path& path) const {
    return [this, stop, path] {
        while (!stop.stop_requested() && backgroundPace(options_.playback(), path) == BackgroundPace::Wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return stop.stop_requested() || !waveformsOn();
    };
}

// The album of the first queued track that needs analysis, else (unless `queuedOnly`) of any such track; a track
// outside an album alone.
std::vector<LoudnessAnalyzer::Track> LoudnessAnalyzer::nextAlbum(bool queuedOnly) {
    auto db = store_.openReader();
    std::optional<int64_t> chosen;
    auto needs = db.prepare("SELECT 1 FROM tracks t JOIN files f ON f.id=t.file_id WHERE t.file_id=? AND " + needsAnalysis());
    for (const auto file : options_.queued()) {
        needs.reset();
        if (!failed_.contains(file) && !away_.contains(file) && needs.bind(1, file).step()) {
            chosen = file;
            break;
        }
    }
    if (!chosen && !queuedOnly) {
        auto any = db.prepare(std::string("SELECT t.file_id FROM tracks t JOIN files f ON f.id=t.file_id WHERE ") +
                              needsAnalysis() + leftOut("t.file_id", failed_, away_) + " ORDER BY t.album_id,t.file_id LIMIT 1");
        if (any.step()) chosen = any.integer(0);
    }
    if (!chosen) return {};
    std::vector<Track> tracks;
    auto members = db.prepare(
        "SELECT f.id,f.path,f.root_id FROM tracks t JOIN files f ON f.id=t.file_id WHERE f.available=1 AND " + notDsd() +
        " AND (t.file_id=?1 OR t.album_id=(SELECT album_id FROM tracks "
        "WHERE file_id=?1)) ORDER BY f.id");
    members.bind(1, *chosen);
    while (members.step())
        tracks.push_back({members.integer(0), std::filesystem::path(toWide(members.text(1))), members.integer(2)});
    return tracks;
}

// A file that is not there went away with its storage; any other that cannot be read stays failed for the session.
void LoudnessAnalyzer::fail(std::set<int64_t>& failed, const Track& track) {
    std::error_code error;
    if (std::filesystem::exists(track.path, error))
        failed.insert(track.file);
    else
        away_[track.file] = track.root;
}

// What failed with its storage away is tried again once its root has been seen offline, as it then is when it is back.
void LoudnessAnalyzer::forgetAway() {
    if (away_.empty()) return;
    std::set<int64_t> offline;
    auto db = store_.openReader();
    for (auto rows = db.prepare("SELECT id FROM roots WHERE online=0"); rows.step();) offline.insert(rows.integer(0));
    std::erase_if(away_, [&](const auto& entry) { return offline.contains(entry.second); });
}

void LoudnessAnalyzer::run(const std::stop_token& stop) {
    using namespace std::chrono_literals;
    auto counted = std::chrono::steady_clock::time_point{};
    // A second's rest after a pass that found nothing to do, gave way or failed; none after one that did its work.
    bool rest = true;
    while (!stop.stop_requested()) {
        if (rest)
            for (int i = 0; i < 10 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
        rest = true;
        gaveWay_ = false;
        try {
            // Counting reads every track, so it runs only while someone reads the progress.
            const auto now = std::chrono::steady_clock::now();
            const auto asked = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(progressAsked_.load()));
            if (now - asked < 5s && now - counted > 10s) {
                count();
                counted = std::chrono::steady_clock::now();
            }
            forgetAway();
            const bool loudness = options_.settings().mode != ReplayGainMode::Off;
            if (!loudness) store_.caughtUp(BackgroundWork::Loudness);
            const bool scanning = store_.scanProgress().scans > 0;
            const bool waveforms = waveformsOn();
            if (!waveforms) store_.caughtUp(BackgroundWork::Waveforms);
            // Queued tracks' loudness first, as playing volume depends on it; then the playing track's waveform; then
            // the rest of the library's loudness; then the background waveforms, after the scan and in their window.
            if (loudness && !scanning && analyzeAlbum(stop, true, waveforms)) {
                rest = gaveWay_;
                continue;
            }
            if (waveforms && waveformsNow(stop)) {
                rest = false;
                continue;
            }
            if (loudness && !scanning && analyzeAlbum(stop, false, waveforms)) {
                count();
                counted = std::chrono::steady_clock::now();
                rest = gaveWay_;
                continue;
            }
            if (loudness && !scanning) store_.caughtUp(BackgroundWork::Loudness);
            if (waveforms && !scanning && inAnalysisWindow(options_.waveform(), minuteOfDay()) && waveformsInBackground(stop)) {
                rest = false;
                continue;
            }
            running_ = false;
        } catch (const std::exception& e) {
            log::warn("audio analysis: %s", e.what());
        }
    }
}

// The next album's loudness, with the waveforms of its files from the same decoding; false when no album needs it.
bool LoudnessAnalyzer::analyzeAlbum(const std::stop_token& stop, bool queuedOnly, bool waveforms) {
    const auto album = nextAlbum(queuedOnly);
    if (album.empty()) return false;
    // Playback comes first (docs/Plan.md §7.7): the analysis waits while the engine is not ahead on the same storage, and
    // goes on with one thread while anything plays; otherwise it takes the scan speed's threads of the cores. An album
    // begun with more threads than either allows starts again with fewer.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const auto allowed = [&] {
        const auto pace = backgroundPace(options_.playback(), album.front().path);
        return pace == BackgroundPace::Wait  ? 0u
               : pace == BackgroundPace::One ? 1u
                                             : store_.backgroundWorkers(BackgroundWork::Loudness, cores);
    };
    const unsigned threads = std::max(1u, allowed());
    const auto yield = [&] {
        if (stop.stop_requested() || options_.settings().mode == ReplayGainMode::Off) return true;
        return threads > allowed();
    };
    running_ = !yield();
    gaveWay_ = !running_;
    if (!running_) return true;
    std::vector<int64_t> ids;
    std::vector<std::filesystem::path> files;
    for (const auto& t : album) {
        ids.push_back(t.file);
        files.push_back(t.path);
    }
    if (!measuring_ || measuringFiles_ != ids) {
        measuring_ = std::make_unique<AlbumMeasure>(files, waveforms);
        measuringFiles_ = ids;
    }
    // Each track's value and waveform are kept as it is measured; the album's value once every track is.
    const bool complete =
        measuring_->run(options_.decoders(), threads, yield, [&](size_t i, const std::optional<Loudness>& track) {
            if (!track) {
                fail(failed_, album[i]);
                return;
            }
            const auto peaks = measuring_->waveform(i);
            store_
                .write([&](sqlite::Database& db) {
                    db.prepare("UPDATE tracks SET rg_analysis_gain=?,rg_analysis_peak=? WHERE file_id=?")
                        .bind(1, track->gainDb)
                        .bind(2, track->peak)
                        .bind(3, ids[i])
                        .run();
                    if (!peaks.empty()) storeWaveform(db, ids[i], peaks);
                })
                .get();
        });
    gaveWay_ = !complete;
    if (!complete) return true;
    const auto result = measuring_->result();
    measuring_.reset();
    if (result.album)
        store_
            .write([&](sqlite::Database& db) {
                auto update = db.prepare("UPDATE tracks SET rg_analysis_album_gain=?,rg_analysis_album_peak=? WHERE file_id=?");
                for (size_t i = 0; i < ids.size(); ++i) {
                    if (!result.tracks[i]) continue;
                    update.reset();
                    update.bind(1, result.album->gainDb).bind(2, result.album->peak).bind(3, ids[i]).run();
                }
            })
            .get();
    return true;
}

// The playing track's waveform, then the next one's, at once whatever the hour or the scan; false when both have one.
bool LoudnessAnalyzer::waveformsNow(const std::stop_token& stop) {
    const auto playing = options_.playing ? options_.playing() : std::vector<PlayingTrack>{};
    {
        // Waveforms of tracks outside the library are kept only while they are playing or next.
        std::lock_guard lock(playingMutex_);
        std::erase_if(playingWaveforms_, [&](const auto& kept) {
            return std::ranges::none_of(playing, [&](const PlayingTrack& t) { return t.path == kept.first; });
        });
    }
    for (const auto& track : playing) {
        if (track.file) {
            if (waveformFailed_.contains(track.file)) continue;
            auto db = store_.openReader();
            if (storedWaveform(db, track.file)) continue;
        } else if (playingWaveform(track.path)) {
            continue;
        }
        running_ = true;
        std::optional<std::vector<uint8_t>> peaks;
        try {
            peaks = readWaveform(track.path, options_.decoders(), givingWay(stop, track.path));
        } catch (const std::exception& e) {
            log::warn("waveform: %s: %s", toUtf8(track.path.wstring()).c_str(), e.what());
        }
        if (stop.stop_requested() || !waveformsOn()) return true;
        if (!peaks) {
            if (track.file) waveformFailed_.insert(track.file);
            continue;
        }
        if (track.file) {
            store_.write([&](sqlite::Database& db) { storeWaveform(db, track.file, *peaks); }).get();
        } else {
            std::lock_guard lock(playingMutex_);
            playingWaveforms_[track.path] = std::move(*peaks);
        }
        return true;
    }
    return false;
}

// The waveforms of library tracks that have none, at the scan speed; each worker finishes its track when the window
// closes, a scan starts or waveforms are turned off, and takes no other. False when every track has one.
bool LoudnessAnalyzer::waveformsInBackground(const std::stop_token& stop) {
    std::vector<Track> tracks;
    {
        auto db = store_.openReader();
        auto rows = db.prepare(std::string("SELECT f.id,f.path,f.root_id FROM tracks t JOIN files f ON f.id=t.file_id "
                                           "LEFT JOIN waveforms w ON w.file_id=f.id WHERE f.available=1 AND ") +
                               rootOnline() + " AND w.file_id IS NULL" + leftOut("f.id", waveformFailed_, away_) +
                               " ORDER BY t.album_id,f.id LIMIT 200");
        while (rows.step()) tracks.push_back({rows.integer(0), std::filesystem::path(toWide(rows.text(1))), rows.integer(2)});
    }
    if (tracks.empty()) {
        store_.caughtUp(BackgroundWork::Waveforms);
        return false;
    }
    running_ = true;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const auto open = [&] {
        return !stop.stop_requested() && waveformsOn() && store_.scanProgress().scans == 0 &&
               inAnalysisWindow(options_.waveform(), minuteOfDay());
    };
    std::atomic<size_t> next = 0;
    std::mutex failedMutex;
    const auto work = [&](unsigned worker) {
        for (size_t at; open() && (at = next++) < tracks.size();) {
            const auto& track = tracks[at];
            // While anything plays, one worker goes on; one on the playing storage waits for the engine to be ahead.
            if (worker > 0 && backgroundPace(options_.playback(), track.path) != BackgroundPace::Full) return;
            if (worker >= store_.backgroundWorkers(BackgroundWork::Waveforms, cores)) return;
            std::optional<std::vector<uint8_t>> peaks;
            try {
                peaks = readWaveform(track.path, options_.decoders(), givingWay(stop, track.path));
            } catch (const std::exception& e) {
                log::warn("waveform: %s: %s", toUtf8(track.path.wstring()).c_str(), e.what());
            }
            if (stop.stop_requested() || !waveformsOn()) return;
            if (!peaks) {
                std::lock_guard lock(failedMutex);
                fail(waveformFailed_, track);
                continue;
            }
            store_.write([&](sqlite::Database& db) { storeWaveform(db, track.file, *peaks); }).get();
        }
    };
    const unsigned threads = store_.backgroundWorkers(BackgroundWork::Waveforms, cores);
    std::vector<std::future<void>> workers;
    for (unsigned i = 1; i < threads && i < tracks.size(); ++i) workers.push_back(std::async(std::launch::async, work, i));
    work(0);
    for (auto& w : workers) w.get();
    return true;
}

} // namespace camel::server
