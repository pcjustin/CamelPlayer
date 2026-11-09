// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <numbers>
#include <thread>
#include <vector>

#include "DsdFixture.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Library/Loudness.h"
#include "Server/Library/Waveform.h"
#include "Common/util/Text.h"
#include "TestUtil.h"
#include "Tools/testaudio/TestAudio.h"

using namespace camel;
using namespace camel::server;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char* name) {
    const auto dir = testutil::tempDir() / "waveform" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

bool waitFor(const std::function<bool()>& check, std::chrono::seconds limit = std::chrono::seconds(20)) {
    const auto end = std::chrono::steady_clock::now() + limit;
    do {
        if (check()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

// `seconds` of a sine at `level` with a full-scale burst from `burstFrom` for a second, at 1 kHz frames for speed.
std::vector<float> signal(double seconds, float level, double burstFrom = -1.0) {
    constexpr uint32_t kRate = 1000;
    std::vector<float> out(size_t(seconds * kRate));
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = double(i) / kRate;
        const float amplitude = burstFrom >= 0 && t >= burstFrom && t < burstFrom + 1.0 ? 1.0f : level;
        out[i] = amplitude * float(std::sin(2.0 * std::numbers::pi * 50.0 * t));
    }
    return out;
}

std::vector<uint8_t> peaksOf(const std::vector<float>& mono) {
    PeakCollector collector(1000, 1);
    collector.add(mono.data(), mono.size());
    return collector.peaks();
}

// A library of `files` already scanned: one root, one folder, a file and a track each.
void addTracks(LibraryStore& store, const std::vector<fs::path>& files) {
    store
        .write([&](sqlite::Database& db) {
            db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/music',0);"
                    "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/music');");
            for (size_t i = 0; i < files.size(); ++i) {
                db.prepare("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES(?,1,1,?,'audio',1,1,1)")
                    .bind(1, int64_t(i + 1))
                    .bind(2, toUtf8(files[i].wstring()))
                    .run();
                db.prepare("INSERT INTO tracks(id,file_id,title,format) VALUES(?,?,'t','FLAC')")
                    .bind(1, int64_t(i + 1))
                    .bind(2, int64_t(i + 1))
                    .run();
            }
        })
        .get();
}

int64_t waveformsIn(LibraryStore& store) {
    return store.openReader().scalar("SELECT COUNT(*) FROM waveforms").value();
}

std::vector<fs::path> flacs(const fs::path& dir, size_t count) {
    std::vector<fs::path> out;
    for (size_t i = 0; i < count; ++i) {
        out.push_back(dir / ("track" + std::to_string(i) + ".flac"));
        fs::copy_file(testutil::generatedFile({"waveform-source", 44100, 16, 2, 1.0}), out.back());
    }
    return out;
}

struct Analysis {
    std::atomic<int> minute = 12 * 60; // noon: outside the default window
    std::atomic<bool> enabled = true;
    ReplayGainMode mode = ReplayGainMode::Off;
    std::vector<LoudnessAnalyzer::PlayingTrack> playing;

    LoudnessAnalyzer::Options options() {
        LoudnessAnalyzer::Options o;
        o.settings = [this] {
            ReplayGainSettings s;
            s.mode = mode;
            return s;
        };
        o.decoders = [] { return DecoderByFormat{}; };
        o.queued = [] { return std::vector<int64_t>{}; };
        o.playback = [] { return PlaybackReads{}; };
        o.waveform = [this] {
            WaveformSettings s;
            s.enabled = enabled;
            return s;
        };
        o.playing = [this] { return playing; };
        o.minuteOfDay = [this] { return minute.load(); };
        return o;
    }
};

} // namespace

TEST(Waveform, SilenceIsFlatAndABurstShowsWhereItIs) {
    const auto silent = peaksOf(std::vector<float>(10000, 0.0f));
    ASSERT_EQ(silent.size(), kWaveformPeaks);
    EXPECT_TRUE(std::ranges::all_of(silent, [](uint8_t p) { return p == 0; }));

    const auto burst = peaksOf(signal(10.0, 0.05f, 5.0)); // 5 s to 6 s of 10
    ASSERT_EQ(burst.size(), kWaveformPeaks);
    EXPECT_EQ(burst[kWaveformPeaks * 55 / 100], 255);
    EXPECT_LT(burst[kWaveformPeaks * 20 / 100], 20);
    EXPECT_LT(burst[kWaveformPeaks * 80 / 100], 20);
    EXPECT_TRUE(PeakCollector(1000, 1).peaks().empty());
}

TEST(Waveform, EachTrackIsScaledToItsOwnLoudest) {
    const auto quiet = peaksOf(signal(10.0, 0.02f));
    const auto loud = peaksOf(signal(10.0, 0.9f));
    EXPECT_GE(*std::ranges::min_element(quiet), 250);
    EXPECT_GE(*std::ranges::min_element(loud), 250);
}

TEST(Waveform, AFileAndADsdFileAreRead) {
    const auto dir = freshDir("read");
    const auto flac = testutil::generatedFile({"waveform-read", 44100, 16, 2, 2.0});
    const auto peaks = readWaveform(flac, {});
    ASSERT_TRUE(peaks.has_value());
    EXPECT_EQ(peaks->size(), kWaveformPeaks);
    EXPECT_EQ(*std::ranges::max_element(*peaks), 255);

    // DSD goes through playback's decimator: a 1 kHz sine at half scale, then the idle pattern.
    auto bytes = dsdfixture::dsdSine(dsdfixture::kDsd64, 1000.0, 0.5, 352800);
    bytes.resize(bytes.size() * 2, 0x69);
    const auto dsf = dir / "tone.dsf";
    dsdfixture::writeFile(dsf, dsdfixture::dsf(bytes, {}));
    const auto dsd = readWaveform(dsf, {});
    ASSERT_TRUE(dsd.has_value());
    EXPECT_EQ(dsd->size(), kWaveformPeaks);
    EXPECT_GE((*dsd)[kWaveformPeaks / 4], 230);    // the tone, in the first half
    EXPECT_LT((*dsd)[kWaveformPeaks * 3 / 4], 30); // silence after it
}

// A FLAC that ends short of its STREAMINFO length, or fails its MD5 at the end, still has what was decoded: playback
// plays it, so its waveform and its loudness are made from it.
TEST(Waveform, AFileThatFailsOnlyAtItsEndIsStillRead) {
    for (const char* name : {"truncated", "damaged"}) {
        const auto file = testutil::testFile(name);
        const auto peaks = readWaveform(file, {});
        ASSERT_TRUE(peaks.has_value()) << name;
        EXPECT_EQ(*std::ranges::max_element(*peaks), 255) << name;
        AlbumMeasure measure({file});
        ASSERT_TRUE(measure.run({}, 1));
        EXPECT_TRUE(measure.result().tracks.front().has_value()) << name;
    }
}

TEST(Waveform, StoredPerFileAndDroppedWhenTheFileChanges) {
    const auto dir = freshDir("store");
    LibraryStore store(dir);
    addTracks(store, flacs(dir, 1));
    const std::vector<uint8_t> peaks(kWaveformPeaks, 7);
    store.write([&](sqlite::Database& db) { storeWaveform(db, 1, peaks); }).get();
    auto db = store.openReader();
    EXPECT_EQ(storedWaveform(db, 1), peaks);
    store.write([](sqlite::Database& w) { w.exec("UPDATE files SET mtime=2 WHERE id=1"); }).get();
    EXPECT_FALSE(storedWaveform(db, 1).has_value());
    store.write([&](sqlite::Database& w) { storeWaveform(w, 99, peaks); }).get(); // no such file: nothing stored
    EXPECT_FALSE(storedWaveform(db, 99).has_value());
}

// Background waveforms wait for their window and then take every track, whatever ReplayGain is set to.
TEST(Waveform, TheBackgroundAnalysisKeepsToItsWindow) {
    const auto dir = freshDir("window");
    LibraryStore store(dir);
    store.setScanSpeed(ScanSpeed::High);
    addTracks(store, flacs(dir, 3));
    Analysis analysis;
    LoudnessAnalyzer analyzer(store, analysis.options());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_EQ(waveformsIn(store), 0); // noon: outside 01:00 to 05:00
    analysis.minute = 2 * 60;
    EXPECT_TRUE(waitFor([&] { return waveformsIn(store) == 3; }));
}

TEST(Waveform, NothingIsMadeWhileWaveformsAreOff) {
    const auto dir = freshDir("off");
    LibraryStore store(dir);
    addTracks(store, flacs(dir, 1));
    Analysis analysis;
    analysis.enabled = false;
    analysis.minute = 2 * 60;
    analysis.playing = {{1, dir / "track0.flac"}};
    LoudnessAnalyzer analyzer(store, analysis.options());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_EQ(waveformsIn(store), 0);
}

// The loudness analysis, at any hour, stores the waveforms of the tracks it decodes.
TEST(Waveform, LoudnessAnalysisMakesWaveformsFromTheSameDecoding) {
    const auto dir = freshDir("loudness");
    LibraryStore store(dir);
    addTracks(store, flacs(dir, 2));
    Analysis analysis;
    analysis.mode = ReplayGainMode::Track;
    LoudnessAnalyzer analyzer(store, analysis.options());
    EXPECT_TRUE(waitFor([&] { return waveformsIn(store) == 2; }));
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM tracks WHERE rg_analysis_gain IS NOT NULL").value(), 2);
}

// A playing track and the next get theirs at once, outside the window; one outside the library is kept, not stored.
TEST(Waveform, PlayingTracksGetTheirsAtOnce) {
    const auto dir = freshDir("playing");
    LibraryStore store(dir);
    const auto files = flacs(dir, 3);
    addTracks(store, files);
    const auto outside = dir / "outside.flac";
    fs::copy_file(files[0], outside);
    Analysis analysis;
    analysis.playing = {{2, files[1]}, {0, outside}};
    LoudnessAnalyzer analyzer(store, analysis.options());
    EXPECT_TRUE(waitFor([&] { return analyzer.playingWaveform(outside).has_value(); }));
    auto db = store.openReader();
    EXPECT_TRUE(storedWaveform(db, 2).has_value());
    EXPECT_FALSE(storedWaveform(db, 1).has_value()); // not playing, and noon is outside the window
    EXPECT_FALSE(storedWaveform(db, 3).has_value());
}
