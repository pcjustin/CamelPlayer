// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ReplayGain (docs/Plan.md §5.9): its settings, the gain a track gets, and loudness analysis with libebur128.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <numbers>
#include <thread>

#include "Common/settings/Settings.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Library/Loudness.h"
#include "Server/Playback/ReplayGain.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;

namespace {

// A 16-bit PCM WAV of a 1 kHz sine at the level `dbfs(seconds)`, the same on every channel.
std::filesystem::path sine(const std::string& name, uint32_t rate, uint16_t channels, const std::function<double(double)>& dbfs,
                           double seconds) {
    std::string out;
    const auto put = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xff));
    };
    const auto frames = uint32_t(rate * seconds);
    const uint32_t data = frames * channels * 2;
    out += "RIFF";
    put(36 + data, 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(1, 2);
    put(channels, 2);
    put(rate, 4);
    put(rate * channels * 2, 4);
    put(channels * 2, 2);
    put(16, 2);
    out += "data";
    put(data, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        const double amplitude = std::pow(10.0, dbfs(double(i) / rate) / 20.0) * 32767.0;
        const auto v = int16_t(std::lround(amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 * i / rate)));
        for (uint16_t c = 0; c < channels; ++c) put(uint16_t(v), 2);
    }
    const auto dir = testutil::tempDir() / "loudness";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(out.data(), std::streamsize(out.size()));
    return path;
}

std::filesystem::path sine(const std::string& name, uint32_t rate, uint16_t channels, double dbfs, double seconds) {
    return sine(name, rate, channels, [dbfs](double) { return dbfs; }, seconds);
}

} // namespace

TEST(ReplayGainSettings, VolumeAndReplayGainRoundTripAndFallBack) {
    const Settings defaults;
    EXPECT_EQ(defaults.volume.mode, VolumeMode::Fixed);
    EXPECT_EQ(volumeDb(defaults.volume), 0.0);
    EXPECT_EQ(defaults.replayGain.mode, ReplayGainMode::Off);
    EXPECT_TRUE(defaults.replayGain.preventClipping);

    Settings settings;
    settings.volume = {VolumeMode::Software, -23.5, true};
    settings.replayGain = {ReplayGainMode::Auto, 3.5, -6.0, false};
    const auto back = fromJson(toJson(settings));
    EXPECT_EQ(back.volume, settings.volume);
    EXPECT_EQ(back.replayGain, settings.replayGain);
    EXPECT_EQ(volumeDb(back.volume), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(volumeDb({VolumeMode::Software, -23.5, false}), -23.5);
    EXPECT_EQ(volumeDb({VolumeMode::Fixed, -23.5, true}), 0.0);

    const auto invalid = fromJson(R"({"volume":{"mode":"loud","db":6},"replayGain":{"mode":"party","preamp":40}})");
    EXPECT_EQ(invalid.volume.mode, VolumeMode::Fixed);
    EXPECT_EQ(invalid.volume.db, 0.0);
    EXPECT_EQ(invalid.replayGain.mode, ReplayGainMode::Off);
    EXPECT_EQ(invalid.replayGain.preampDb, 0.0);
    EXPECT_EQ(resetPlayback(settings).replayGain, ReplayGainSettings{});
}

TEST(ReplayGain, TheModePicksTrackOrAlbumAndThePeakLimitsTheGain) {
    const TrackLoudness both{-6.0, 0.5, -8.0, 0.9};
    ReplayGainSettings settings;
    EXPECT_EQ(replayGainDb(settings, both, true), 0.0); // off
    settings.mode = ReplayGainMode::Track;
    EXPECT_EQ(replayGainDb(settings, both, true), -6.0);
    settings.mode = ReplayGainMode::Album;
    EXPECT_EQ(replayGainDb(settings, both, false), -8.0);
    settings.mode = ReplayGainMode::Auto;
    EXPECT_EQ(replayGainDb(settings, both, true), -8.0);
    EXPECT_EQ(replayGainDb(settings, both, false), -6.0);
    // An album without a value falls back to the track's.
    EXPECT_EQ(replayGainDb(settings, {-6.0, 0.5, std::nullopt, std::nullopt}, true), -6.0);

    // The preamp adds to a value; a track without one gets its own preamp.
    settings.mode = ReplayGainMode::Track;
    settings.preampDb = 3.0;
    settings.noValuePreampDb = -4.0;
    EXPECT_EQ(replayGainDb(settings, both, false), -3.0);
    EXPECT_EQ(replayGainDb(settings, {}, false), -4.0);

    // A quiet track's boost stops where its peak reaches full scale: a peak of 0.5 allows 6.02 dB.
    const TrackLoudness quiet{+10.0, 0.5, std::nullopt, std::nullopt};
    settings.preampDb = 0.0;
    EXPECT_NEAR(replayGainDb(settings, quiet, false), 20.0 * std::log10(2.0), 1e-9);
    settings.preventClipping = false;
    EXPECT_EQ(replayGainDb(settings, quiet, false), 10.0);
}

TEST(Loudness, AnEbuReferenceToneMeasuresMinus23LufsAndMonoCountsAsDualMono) {
    // EBU Tech 3341 test 1: a 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS.
    const auto stereo = sine("tone-stereo.wav", 48000, 2, -23.0, 20.0);
    const auto mono = sine("tone-mono.wav", 48000, 1, -23.0, 20.0);
    const auto quieter = sine("tone-quieter.wav", 44100, 2, -33.0, 20.0);
    AlbumMeasure measure({stereo, mono, quieter});
    EXPECT_TRUE(measure.run({}, 2));
    const auto result = measure.result();
    ASSERT_EQ(result.tracks.size(), 3u);
    ASSERT_TRUE(result.tracks[0] && result.tracks[1] && result.tracks[2]);
    EXPECT_NEAR(result.tracks[0]->gainDb, kReferenceLufs + 23.0, 0.1);
    EXPECT_NEAR(result.tracks[1]->gainDb, kReferenceLufs + 23.0, 0.1);
    EXPECT_NEAR(result.tracks[2]->gainDb, kReferenceLufs + 33.0, 0.1);
    EXPECT_NEAR(result.tracks[0]->peak, std::pow(10.0, -23.0 / 20.0), 0.002);
    // The album is the three together: louder than its quietest track, with the loudest peak.
    ASSERT_TRUE(result.album);
    EXPECT_GT(result.album->gainDb, result.tracks[0]->gainDb);
    EXPECT_LT(result.album->gainDb, result.tracks[2]->gainDb);
    EXPECT_DOUBLE_EQ(result.album->peak, std::max({result.tracks[0]->peak, result.tracks[1]->peak, result.tracks[2]->peak}));
}

TEST(Loudness, AFileThatCannotBeReadIsLeftOutOfTheAlbum) {
    const auto tone = sine("tone-alone.wav", 48000, 2, -23.0, 5.0);
    const auto broken = testutil::tempDir() / "loudness" / "broken.wav";
    std::ofstream(broken, std::ios::binary | std::ios::trunc) << "not a wave file";
    AlbumMeasure measure({tone, broken});
    EXPECT_TRUE(measure.run({}, 1));
    const auto result = measure.result();
    ASSERT_EQ(result.tracks.size(), 2u);
    EXPECT_TRUE(result.tracks[0]);
    EXPECT_FALSE(result.tracks[1]);
    ASSERT_TRUE(result.album);
    EXPECT_NEAR(result.album->gainDb, result.tracks[0]->gainDb, 1e-9);
}

TEST(Loudness, TheAlbumGatesItsBlocksAsLibebur128GatesATrack) {
    // Loud, then quiet enough for the relative gate, then between, with changes off the 100 ms grid.
    const auto level = [](double t) {
        if (t < 3.37) return -20.0;
        return t < 6.81 ? -45.0 : -28.0;
    };
    const auto steps = sine("tone-steps.wav", 44100, 2, level, 10.0);
    AlbumMeasure measure({steps});
    ASSERT_TRUE(measure.run({}, 1));
    const auto result = measure.result();
    ASSERT_TRUE(result.tracks[0] && result.album);
    EXPECT_NEAR(result.album->gainDb, result.tracks[0]->gainDb, 1e-6);
}

TEST(Loudness, AnAlbumCutShortGoesOnWhereItStopped) {
    const std::vector<std::filesystem::path> files{sine("cut-1.wav", 48000, 2, -20.0, 4.0),
                                                   sine("cut-2.wav", 48000, 2, -30.0, 4.0),
                                                   sine("cut-3.wav", 44100, 1, -25.0, 4.0)};
    AlbumMeasure whole(files);
    ASSERT_TRUE(whole.run({}, 1));

    AlbumMeasure cut(files);
    std::vector<size_t> measured;
    const auto record = [&](size_t i, const std::optional<Loudness>& track) {
        EXPECT_TRUE(track);
        measured.push_back(i);
    };
    EXPECT_FALSE(cut.run({}, 1, [&] { return !measured.empty(); }, record));
    EXPECT_EQ(measured, std::vector<size_t>{0});
    EXPECT_TRUE(cut.result().tracks[0]);
    EXPECT_FALSE(cut.result().tracks[1]);
    EXPECT_TRUE(cut.run({}, 1, {}, record));
    EXPECT_EQ(measured, (std::vector<size_t>{0, 1, 2}));
    const auto expected = whole.result();
    const auto result = cut.result();
    for (size_t i = 0; i < files.size(); ++i) EXPECT_DOUBLE_EQ(result.tracks[i]->gainDb, expected.tracks[i]->gainDb) << i;
    EXPECT_DOUBLE_EQ(result.album->gainDb, expected.album->gainDb);
    EXPECT_DOUBLE_EQ(result.album->peak, expected.album->peak);
}

// Counting reads every track of the library, so it waits until someone reads the progress (the Settings page).
// DSD is not analyzed, so it is not counted either: with it the analysis would never reach its total.
TEST(Loudness, DsdTracksAreLeftOutOfTheAnalysis) {
    const auto directory = testutil::tempDir() / "loudness-dsd";
    std::filesystem::remove_all(directory);
    LibraryStore store(directory);
    store
        .write([](sqlite::Database& db) {
            db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/music',0);"
                    "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/music');"
                    "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES"
                    "(1,1,1,'C:/music/one.flac','audio',1,1,1),(2,1,1,'C:/music/two.dsf','audio',1,1,1);");
            db.prepare("INSERT INTO tracks(id,file_id,title,format) VALUES(1,1,'One',?),(2,2,'Two',?)")
                .bind(1, std::string(audio::formatId(audio::AudioFormat::Flac)))
                .bind(2, std::string(audio::formatId(audio::AudioFormat::Dsd)))
                .run();
        })
        .get();
    LoudnessAnalyzer analyzer(store, {[] { return ReplayGainSettings{}; }, [] { return DecoderByFormat{}; },
                                      [] { return std::vector<int64_t>{}; }, [] { return PlaybackReads{}; }});
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (analyzer.progress().total == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(analyzer.progress().total, 1);
}

TEST(Loudness, TheAnalyzerCountsTracksOnlyWhileItsProgressIsRead) {
    const auto directory = testutil::tempDir() / "loudness-count";
    std::filesystem::remove_all(directory);
    LibraryStore store(directory);
    store
        .write([](sqlite::Database& db) {
            db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/music',0);"
                    "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/music');"
                    "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) "
                    "VALUES(1,1,1,'C:/music/one.flac','audio',1,1,1);"
                    "INSERT INTO tracks(id,file_id,title,format) VALUES(1,1,'One','FLAC');");
        })
        .get();
    LoudnessAnalyzer analyzer(store, {[] { return ReplayGainSettings{}; }, [] { return DecoderByFormat{}; },
                                      [] { return std::vector<int64_t>{}; }, [] { return PlaybackReads{}; }});
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_EQ(analyzer.progress().total, 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (analyzer.progress().total == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(analyzer.progress().total, 1);
}
