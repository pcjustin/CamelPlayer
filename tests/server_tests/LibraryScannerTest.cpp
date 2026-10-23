// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <fstream>
#include <thread>
#include <wil/resource.h>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>

#include "Server/Library/LibraryScanner.h"
#include "Common/DecoderRegistry.h"
#include "DsdFixture.h"
#include "LameFixture.h"
#include "TestUtil.h"

using namespace camel::server;
namespace fs = std::filesystem;

namespace {

class LibraryScannerTest : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        dir = testutil::tempDir() / "scanner" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::remove_all(dir);
        fs::create_directories(dir / "music" / "album");
    }
    fs::path audio(const char* name = "one.flac") {
        const auto dest = dir / "music" / "album" / name;
        fs::copy_file(testutil::testFile("16-44.1k"), dest, fs::copy_options::overwrite_existing);
        return dest;
    }
    static int64_t count(LibraryStore& store, const char* sql = "SELECT COUNT(*) FROM files WHERE available=1") {
        return store.openReader().scalar(sql).value();
    }
    static bool waitFor(const std::function<bool()>& check) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do {
            if (check()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }
};

TEST_F(LibraryScannerTest, NormalizesAliasesAndOverlappingRootsInBothOrders) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto child = scanner.addRoot(dir / "music" / "album");
    EXPECT_TRUE(scanner.scan(child).complete);
    const auto parent = scanner.addRoot(dir / "music");
    EXPECT_NE(child, parent);
    EXPECT_EQ(scanner.addRoot(dir / "music" / "." / "album" / ".."), parent);
    EXPECT_EQ(scanner.addRoot(dir / "music" / "album"), parent);
    EXPECT_TRUE(scanner.scan(parent).complete);
    EXPECT_EQ(count(store), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM roots WHERE removed IS NULL"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks"), 1);
    EXPECT_THROW(scanner.scan(child), std::invalid_argument);
    EXPECT_THROW(scanner.addRoot(dir / "missing"), std::runtime_error);
    EXPECT_THROW(scanner.addRoot(dir / "music" / "album" / "one.flac"), std::invalid_argument);
}

TEST_F(LibraryScannerTest, ReadsTagsValuesFormatAndEmbeddedCoverAndSkipsUnchangedMetadata) {
    const auto file = audio();
    {
        TagLib::FLAC::File tags(file.c_str());
        TagLib::PropertyMap values;
        values.insert("TITLE", TagLib::StringList(TagLib::String("測試", TagLib::String::UTF8)));
        values.insert("ALBUM", TagLib::StringList("Album"));
        values.insert("ALBUMARTIST", TagLib::StringList("Album artist"));
        TagLib::StringList artists("First");
        artists.append("Second");
        values.insert("ARTIST", artists);
        values.insert("TRACKNUMBER", TagLib::StringList("2/10"));
        values.insert("DISCNUMBER", TagLib::StringList("1/2"));
        values.insert("DATE", TagLib::StringList("2024-10-23"));
        values.insert("DISCTOTAL", TagLib::StringList("invalid"));
        values.insert("LYRICS", TagLib::StringList("Words"));
        values.insert("REPLAYGAIN_TRACK_GAIN", TagLib::StringList("-3.5 dB"));
        tags.setProperties(values);
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setData(TagLib::ByteVector("png", 3));
        tags.addPicture(picture);
        ASSERT_TRUE(tags.save());
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    const auto first = scanner.scan(root);
    ASSERT_TRUE(first.complete);
    EXPECT_EQ(first.updated, 1);
    auto db = store.openReader();
    auto row = db.prepare("SELECT title,format,sample_rate,bits_per_sample,channels,total_frames,embedded_cover,lyrics,"
                          "track_number,track_total,disc_total,rg_track_gain,year FROM tracks");
    ASSERT_TRUE(row.step());
    EXPECT_EQ(row.text(0), "測試");
    EXPECT_EQ(row.text(1), "flac");
    EXPECT_EQ(row.integer(2), 44100);
    EXPECT_EQ(row.integer(3), 16);
    EXPECT_EQ(row.integer(4), 2);
    EXPECT_EQ(row.integer(5), 88200);
    EXPECT_EQ(row.integer(6), 1);
    EXPECT_EQ(row.text(7), "embedded");
    EXPECT_EQ(row.integer(8), 2);
    EXPECT_EQ(row.integer(9), 10);
    EXPECT_EQ(row.integer(10), 2);
    EXPECT_DOUBLE_EQ(row.real(11), -3.5);
    EXPECT_EQ(row.integer(12), 2024);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM track_values WHERE field='artist'"), 2);
    EXPECT_EQ(scanner.scan(root).updated, 0);
    store.write([](sqlite::Database& w) { w.exec("INSERT INTO stats(file_id,rating) SELECT id,5 FROM files"); }).get();
    {
        TagLib::FLAC::File tags(file.c_str());
        tags.tag()->setTitle("Changed title");
        ASSERT_TRUE(tags.save());
    }
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    EXPECT_EQ(scanner.scan(root).updated, 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE title='Changed title'"), 1);
    EXPECT_EQ(count(store, "SELECT rating FROM stats"), 5);
}

TEST_F(LibraryScannerTest, IndexesSidecarsAndEnforcesContentAndDepthScope) {
    audio("renamed.mp3"); // content decides the codec
    for (const char* name : {"disc.cue", "cover.JPG", "words.lrc", "ignored.txt", "ignored.ogg"})
        std::ofstream(dir / "music" / "album" / name) << "test";
    std::ofstream(dir / "music" / "album" / "invalid.flac") << "not audio";
    const camel::audio::testaudio::GenSpec spec{"32bit", 44100, 32, 2, 0.05};
    camel::audio::testaudio::writeFlac(dir / "music" / "album" / "32.flac", spec);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto report = scanner.scan(scanner.addRoot(dir / "music"));
    EXPECT_TRUE(report.complete);
    EXPECT_EQ(report.updated, 1);
    EXPECT_EQ(report.diagnostics.size(), 2);
    EXPECT_EQ(count(store), 6);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE format='flac'"), 1);
}

TEST_F(LibraryScannerTest, OfflineCancelledAndPartialScansRetainUnseenFiles) {
    const auto file = audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    fs::remove(file);
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_FALSE(scanner.scan(root, cancelled).complete);
    EXPECT_EQ(count(store), 1);
    // A sharing violation simulates an unreadable file during an otherwise accessible scan.
    const auto blocked = audio("blocked.flac");
    HANDLE handle = CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto partial = scanner.scan(root);
    CloseHandle(handle);
    EXPECT_FALSE(partial.complete);
    EXPECT_EQ(count(store), 1);
    fs::rename(dir / "music", dir / "offline");
    const auto offline = scanner.scan(root);
    EXPECT_FALSE(offline.complete);
    EXPECT_FALSE(offline.online);
    EXPECT_NE(offline.rootError, 0);
    EXPECT_EQ(count(store), 1);
    EXPECT_EQ(count(store, "SELECT online FROM roots WHERE removed IS NULL"), 0);
    fs::rename(dir / "offline", dir / "music");
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store), 1); // blocked.flac replaces the now-confirmed missing one
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE available=0"), 1);
}

TEST_F(LibraryScannerTest, ReadsEverySupportedContainer) {
    using namespace camel::audio::testaudio;
    audio();
    const GenSpec spec{"short", 44100, 16, 2, 0.1};
    const auto pcm = makeSignal(spec);
    const auto folder = dir / "music" / "album";
    writeWav(folder / "wave.wav", pcm, 44100, 16, 2);
    writeAiff(folder / "aiff.aiff", pcm, 44100, 16, 2);
    writeAlac(folder / "alac.m4a", pcm, 44100, 16, 2);
    writeAac(folder / "aac.m4a", pcm, 44100, 16, 2, false);
    writeAac(folder / "adts.aac", pcm, 44100, 16, 2, true);
    testutil::writeMp3(folder / "mp3.mp3", pcm, 44100, 16, 2, true);
    const auto raw = dsdfixture::rawDsd(4096, 2);
    dsdfixture::writeFile(folder / "dsf.dsf", dsdfixture::dsf(raw, {}));
    dsdfixture::writeFile(folder / "dff.dff", dsdfixture::dff(raw, 2, dsdfixture::kDsd64, false));
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto report = scanner.scan(scanner.addRoot(dir / "music"));
    for (const auto& diagnostic : report.diagnostics) ADD_FAILURE() << diagnostic;
    EXPECT_TRUE(report.complete);
    EXPECT_EQ(report.updated, 9);
    EXPECT_EQ(count(store, "SELECT COUNT(DISTINCT format) FROM tracks"), 7);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE format='dsd' AND sample_rate=2822400 "
                           "AND bits_per_sample=1 AND total_frames=32768"),
              2);
    EXPECT_EQ(scanner.scan(scanner.addRoot(dir / "music")).updated, 0);
}

TEST_F(LibraryScannerTest, CancellationAfterAPublishedBatchRetainsUnseenFiles) {
    const auto file = audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    fs::remove(file);
    for (int folder = 0; folder < 60; folder++) {
        const auto path = dir / "music" / std::to_string(folder);
        fs::create_directories(path);
        for (int i = 0; i < 25; i++) std::ofstream(path / (std::to_string(i) + ".lrc")) << "words";
    }
    CancellationToken token;
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root, token); });
    ASSERT_TRUE(waitFor([&] { return count(store) >= 501; }));
    token.cancel();
    const auto report = job.get();
    EXPECT_FALSE(report.complete);
    EXPECT_GE(report.seen, 500);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE kind='audio' AND available=1"), 1);
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store), 1500);
}

TEST_F(LibraryScannerTest, SlowDirectoryDoesNotBlockCompletedWorkers) {
    auto decoder = camel::audio::openDecoder(camel::audio::DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    auto& registry = camel::audio::decoderRegistry();
    const auto id = camel::audio::decoderId(camel::audio::DecoderKind::Sndfile);
    const auto opener = registry.find(id)->open;
    auto restore = wil::scope_exit([&] { registry.setOpener(id, opener); });
    std::promise<void> release, started;
    auto gate = release.get_future().share();
    registry.setOpener(id, [&, opener](const fs::path& path, std::string_view format, uint32_t bits) {
        if (path.filename() == "slow.flac") {
            started.set_value();
            gate.wait();
        }
        return opener(path, format, bits);
    });
    fs::create_directory(dir / "music" / "00-slow");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "00-slow" / "slow.flac");
    fs::create_directory(dir / "music" / "01-fast");
    for (int i = 0; i < 500; i++) std::ofstream(dir / "music" / "01-fast" / (std::to_string(i) + ".lrc")) << "words";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root); });
    bool released = false;
    auto unblock = wil::scope_exit([&] {
        if (!released) release.set_value();
    });
    EXPECT_EQ(started.get_future().wait_for(std::chrono::seconds(8)), std::future_status::ready);
    EXPECT_TRUE(waitFor([&] { return count(store) == 500; }));
    release.set_value();
    released = true;
    EXPECT_TRUE(job.get().complete);
    EXPECT_EQ(count(store), 501);
}

TEST_F(LibraryScannerTest, ReparseDirectoriesDoNotEscapeOrLoop) {
    audio();
    fs::create_directories(dir / "outside");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "outside" / "outside.flac");
    // Junctions do not require the symbolic-link privilege.
    const auto junction = [&](const fs::path& link, const fs::path& target) {
        const auto command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
        return _wsystem(command.c_str()) == 0;
    };
    auto removeLinks = wil::scope_exit([&] {
        std::error_code error;
        fs::remove(dir / "music" / "escape", error);
        fs::remove(dir / "music" / "album" / "cycle", error);
        fs::remove(dir / "music", error);
    });
    ASSERT_TRUE(junction(dir / "music" / "escape", dir / "outside"));
    ASSERT_TRUE(junction(dir / "music" / "album" / "cycle", dir / "music"));
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    EXPECT_EQ(scanner.addRoot(dir / "music" / "album" / "cycle"), root);
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store), 1);
    fs::remove(dir / "music" / "escape");
    fs::remove(dir / "music" / "album" / "cycle");
    fs::rename(dir / "music", dir / "original");
    ASSERT_TRUE(junction(dir / "music", dir / "outside"));
    EXPECT_FALSE(scanner.scan(root).complete);
    EXPECT_EQ(count(store), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks"), 1);
}

TEST_F(LibraryScannerTest, MonitorScansChangesAndReconnectsAnOfflineRoot) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(0);
    options.offlineInterval = std::chrono::milliseconds(100);
    options.coalesce = std::chrono::milliseconds(100);
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    audio("two.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 2; }));
    fs::remove(dir / "music" / "album" / "one.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    fs::rename(dir / "music", dir / "offline");
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots WHERE removed IS NULL") == 0; }));
    EXPECT_EQ(count(store), 1);
    fs::rename(dir / "offline", dir / "music");
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots WHERE removed IS NULL") == 1; }));
    EXPECT_EQ(count(store), 1);
}

TEST_F(LibraryScannerTest, PeriodicScanRunsWithoutANotification) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(150);
    options.offlineInterval = std::chrono::milliseconds(50);
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL") == 1; }));
    store.write([](sqlite::Database& db) { db.exec("UPDATE roots SET online=0"); }).get();
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots WHERE removed IS NULL") == 1; }));
}

TEST_F(LibraryScannerTest, AttributeNotificationTriggersAnIncrementalScan) {
    const auto file = audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(0);
    options.offlineInterval = std::chrono::milliseconds(100);
    options.coalesce = std::chrono::milliseconds(100);
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    const auto before = count(store, "SELECT MAX(last_seen) FROM files");
    const DWORD attributes = GetFileAttributesW(file.c_str());
    auto restore = wil::scope_exit([&] { SetFileAttributesW(file.c_str(), attributes); });
    ASSERT_TRUE(SetFileAttributesW(file.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN));
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT MAX(last_seen) FROM files") > before; }));
    EXPECT_EQ(count(store), 1);
}

} // namespace
