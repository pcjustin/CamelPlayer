// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <fstream>
#include <algorithm>
#include <thread>
#include <wil/resource.h>

#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>

#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryText.h"
#include "Common/DecoderRegistry.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/decode/FormatProbe.h"
#include "DsdFixture.h"
#include "LameFixture.h"
#include "Common/util/Text.h"
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

TEST_F(LibraryScannerTest, ACompactDateYieldsItsYear) {
    const auto file = audio();
    {
        TagLib::FLAC::File tags(file.c_str());
        TagLib::PropertyMap values;
        values.insert("DATE", TagLib::StringList("20200501"));
        tags.setProperties(values);
        ASSERT_TRUE(tags.save());
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    EXPECT_EQ(count(store, "SELECT year FROM tracks"), 2020);
}

TEST_F(LibraryScannerTest, TagValuesLoseSurroundingBlanksAndBlankValuesAreSkipped) {
    const auto file = audio();
    {
        TagLib::FLAC::File tags(file.c_str());
        TagLib::PropertyMap values;
        values.insert("TITLE", TagLib::StringList(TagLib::String("\t Title \r\n", TagLib::String::UTF8)));
        TagLib::StringList artists(TagLib::String(" Dragon Ash", TagLib::String::UTF8));
        artists.append(TagLib::String("\xE3\x80\x80", TagLib::String::UTF8));
        artists.append(TagLib::String("\tステファニー\xE3\x80\x80", TagLib::String::UTF8));
        values.insert("ARTIST", artists);
        values.insert("COMPOSER", TagLib::StringList(TagLib::String("   ", TagLib::String::UTF8)));
        tags.setProperties(values);
        ASSERT_TRUE(tags.save());
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    auto db = store.openReader();
    auto title = db.prepare("SELECT title FROM tracks");
    ASSERT_TRUE(title.step());
    EXPECT_EQ(title.text(0), "Title");
    auto artists = db.prepare("SELECT value FROM track_values WHERE field='artist' ORDER BY position");
    std::vector<std::string> names;
    while (artists.step()) names.push_back(artists.text(0));
    EXPECT_EQ(names, (std::vector<std::string>{"Dragon Ash", "ステファニー"}));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM track_values WHERE field='composer'"), 0);
}

TEST_F(LibraryScannerTest, ReadsTagsValuesFormatAndEmbeddedCoverAndSkipsUnchangedMetadata) {
    const auto file = audio();
    {
        TagLib::FLAC::File tags(file.c_str());
        TagLib::PropertyMap values;
        values.insert("TITLE", TagLib::StringList(TagLib::String("測試", TagLib::String::UTF8)));
        values.insert("TITLESORT", TagLib::StringList("Sort title"));
        values.insert("ALBUM", TagLib::StringList("Album"));
        values.insert("ALBUMARTIST", TagLib::StringList("Album artist"));
        TagLib::StringList artists("First");
        artists.append("Second");
        values.insert("ARTIST", artists);
        TagLib::StringList artistSorts("Z first");
        artistSorts.append("A second");
        values.insert("ARTISTSORT", artistSorts);
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
                          "track_number,track_total,disc_total,rg_track_gain,year,title_sort,title_norm FROM tracks");
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
    EXPECT_EQ(row.blob(13), librarySortKey("Sort title"));
    EXPECT_EQ(row.text(14), "测试");
    auto sortedValues = db.prepare("SELECT sort_key FROM track_values WHERE field='artist' ORDER BY position");
    ASSERT_TRUE(sortedValues.step());
    EXPECT_EQ(sortedValues.blob(0), librarySortKey("Z first"));
    ASSERT_TRUE(sortedValues.step());
    EXPECT_EQ(sortedValues.blob(0), librarySortKey("A second"));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM track_values WHERE field='artist'"), 2);
    auto albumDb = store.openReader();
    auto album = albumDb.prepare("SELECT a.title,a.album_artist,t.album_id,c.source,a.disc_count FROM tracks t "
                                 "JOIN albums a ON a.id=t.album_id LEFT JOIN covers c ON c.id=a.cover_id");
    ASSERT_TRUE(album.step());
    EXPECT_EQ(album.text(0), "Album");
    EXPECT_EQ(album.text(1), "Album artist");
    EXPECT_EQ(album.text(3), "embedded");
    EXPECT_EQ(album.integer(4), 2);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums"), 1);
    EXPECT_EQ(scanner.scan(root).updated, 0);
    {
        std::ofstream cover(dir / "music" / "album" / "cover.jpg", std::ios::binary);
        cover << "folder cover";
    }
    ASSERT_TRUE(scanner.scan(root).complete);
    auto coverDb = store.openReader();
    auto cover = coverDb.prepare("SELECT c.source FROM albums a JOIN covers c ON c.id=a.cover_id");
    ASSERT_TRUE(cover.step());
    EXPECT_EQ(cover.text(0), "folder");
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

TEST_F(LibraryScannerTest, GroupsDiscFoldersButKeepsEditionsSeparate) {
    const auto cd1 = dir / "music" / "album" / "CD1";
    const auto cd2 = dir / "music" / "album" / "CD2";
    const auto edition = dir / "music" / "album-remaster";
    fs::create_directories(cd1);
    fs::create_directories(cd2);
    fs::create_directories(edition);
    const auto addTrack = [&](const fs::path& path, const char* albumName, const char* artist) {
        fs::copy_file(testutil::testFile("16-44.1k"), path);
        TagLib::FLAC::File tags(path.c_str());
        TagLib::PropertyMap values;
        values.insert("ALBUM", TagLib::StringList(albumName));
        values.insert("ARTIST", TagLib::StringList(artist));
        ASSERT_TRUE(tags.setProperties(values).isEmpty());
        ASSERT_TRUE(tags.save());
    };
    addTrack(cd1 / "one.flac", "CD1 Collection [Disc 1]", "Artist One");
    addTrack(cd2 / "two.flac", "CD1 Collection [Disc 2]", "Artist Two");
    addTrack(edition / "three.flac", "CD1 Collection", "Artist One");

    LibraryStore store(dir / "library.db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    auto db = store.openReader();
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM albums WHERE title='CD1 Collection'").value(), 2);
    auto merged = db.prepare("SELECT a.album_artist,COUNT(t.id) FROM albums a JOIN tracks t ON t.album_id=a.id "
                             "WHERE a.title='CD1 Collection' GROUP BY a.id ORDER BY COUNT(t.id) DESC");
    ASSERT_TRUE(merged.step());
    EXPECT_EQ(merged.text(0), "Various Artists");
    EXPECT_EQ(merged.integer(1), 2);
    auto discs = db.prepare("SELECT t.disc_number FROM tracks t JOIN files f ON f.id=t.file_id WHERE f.path LIKE ?");
    discs.bind(1, "%one.flac");
    ASSERT_TRUE(discs.step());
    EXPECT_EQ(discs.integer(0), 1);
    discs.reset();
    discs.bind(1, "%two.flac");
    ASSERT_TRUE(discs.step());
    EXPECT_EQ(discs.integer(0), 2);
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
    EXPECT_EQ(count(store), 5); // CUE sheets are not indexed
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE format='flac'"), 1);
}

TEST_F(LibraryScannerTest, FlacMetadataDoesNotOpenAPlaybackDecoder) {
    using namespace camel::audio;
    audio();
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::atomic<int> opens = 0;
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        ++opens;
        return original(path, format, bits);
    });
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    EXPECT_EQ(opens.load(), 0);
    EXPECT_EQ(count(store, "SELECT total_frames FROM tracks"), 88200);
}

TEST_F(LibraryScannerTest, FlacMetadataSkipsLargeArtworkAndRejectsTruncatedBlocks) {
    const auto path = audio();
    {
        TagLib::FLAC::File tags(path.c_str());
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setData(TagLib::ByteVector(8 << 20, 'x'));
        tags.addPicture(picture);
        tags.tag()->setTitle("Large cover");
        ASSERT_TRUE(tags.save());
    }
    const auto truncated = path.parent_path() / "truncated.flac";
    fs::copy_file(path, truncated);
    fs::resize_file(truncated, 64);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto report = scanner.scan(scanner.addRoot(dir / "music"));
    EXPECT_TRUE(report.complete);
    EXPECT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE embedded_cover=1 AND title='Large cover' AND total_frames=88200"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks"), 1);
}

TEST_F(LibraryScannerTest, ExternalArtworkIsIndexedWithoutReadingItsPayload) {
    audio();
    const auto cover = dir / "music" / "album" / "cover.jpg";
    {
        std::ofstream output(cover, std::ios::binary);
        const std::string bytes(8 << 20, 'x');
        output.write(bytes.data(), std::streamsize(bytes.size()));
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    IO_COUNTERS before{}, after{};
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM covers WHERE source='folder'"), 1);
    EXPECT_LT(after.ReadTransferCount - before.ReadTransferCount, 2ull << 20);
}

TEST_F(LibraryScannerTest, Mp3MetadataUsesDeclaredLengthsAndEstimatesTheRestWithoutReadingThem) {
    using namespace camel::audio;
    using namespace camel::audio::testaudio;
    const auto pcm = makeSignal({"short", 44100, 16, 2, 10.0});
    for (const bool header : {true, false}) {
        const auto path = dir / "music" / "album" / (header ? "tagged.mp3" : "plain.mp3");
        testutil::writeMp3(path, pcm, 44100, 16, 2, header);
        IO_COUNTERS before{}, after{};
        ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
        bool estimated = !header;
        const auto info = probeMp3Info(path, estimated);
        ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
        EXPECT_LT(after.ReadTransferCount - before.ReadTransferCount, fs::file_size(path) / 2);
        EXPECT_EQ(estimated, !header);
        const auto exact = openDecoder(DecoderKind::Mpg123, path)->info().totalFrames;
        if (header) EXPECT_EQ(info.totalFrames, exact);
        else EXPECT_NEAR(double(info.totalFrames), double(exact), exact * 0.02);
        EXPECT_EQ(info.sampleRate, 44100u);
        EXPECT_EQ(info.channels, 2u);
        EXPECT_EQ(info.bitsPerSample, 24u);
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks t JOIN files f ON f.id=t.file_id "
                           "WHERE f.path LIKE '%plain.mp3' AND t.length_estimated=1"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE length_estimated=1"), 1);
    const auto plain = count(store, "SELECT id FROM files WHERE path LIKE '%plain.mp3'");
    const auto revision = count(store, "SELECT value FROM library_revision WHERE id=1");
    scanner.recordExactLength(plain, 48000, 1);
    scanner.recordExactLength(plain, 44100, 441000);
    store.write([](sqlite::Database&) {}).get();
    const auto corrected = "SELECT total_frames FROM tracks WHERE length_estimated=0 AND file_id=" + std::to_string(plain);
    EXPECT_EQ(count(store, corrected.c_str()), 441000);
    EXPECT_GT(count(store, "SELECT value FROM library_revision WHERE id=1"), revision);
    scanner.recordExactLength(plain, 44100, 7);
    store.write([](sqlite::Database&) {}).get();
    EXPECT_EQ(count(store, corrected.c_str()), 441000);
}

TEST_F(LibraryScannerTest, UnchangedScanKeepsTheBrowseRevisionAndDerivedWorkResumesAfterFailure) {
    const auto path = audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto revision = count(store, "SELECT value FROM library_revision WHERE id=1");
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT value FROM library_revision WHERE id=1"), revision);
    {
        TagLib::FLAC::File tags(path.c_str());
        tags.tag()->setAlbum("Changed album");
        ASSERT_TRUE(tags.save());
    }
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    const auto blocked = audio("blocked.flac");
    wil::unique_handle handle(CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(handle);
    EXPECT_FALSE(scanner.scan(root).complete);
    handle.reset();
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums WHERE title='Changed album'"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM app_state WHERE key LIKE 'library_derived_dirty:%'"), 0);
}

TEST_F(LibraryScannerTest, AnAllTagsRereadKeepsIdentityAndFingerprint) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto title = [&] {
        auto row = store.openReader().prepare("SELECT title FROM tracks");
        return row.step() ? row.text(0) : std::string{};
    };
    const std::string tagged = title();
    const auto file = count(store, "SELECT id FROM files WHERE kind='audio'");
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='Stale'; UPDATE files SET content_hash=zeroblob(32)"); })
        .get();
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(title(), "Stale"); // an unchanged file is not read again

    requestMetadataReread(store);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(title(), tagged);
    EXPECT_EQ(count(store, "SELECT id FROM files WHERE kind='audio'"), file);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE length(content_hash)=32"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM app_state WHERE key LIKE 'library_reread:%'"), 0);
}

TEST_F(LibraryScannerTest, OneChangedFileDoesNotCountEveryTrackAsALibraryChange) {
    constexpr int kFolders = 4, kFiles = 5;
    for (int folder = 0; folder < kFolders; ++folder) {
        fs::create_directories(dir / "music" / ("album" + std::to_string(folder)));
        for (int file = 0; file < kFiles; ++file)
            fs::copy_file(testutil::testFile("16-44.1k"),
                          dir / "music" / ("album" + std::to_string(folder)) / (std::to_string(file) + ".flac"));
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE album_id IS NOT NULL"), kFolders * kFiles);
    const auto revision = count(store, "SELECT value FROM library_revision WHERE id=1");

    const auto changed = dir / "music" / "album0" / "0.flac";
    {
        TagLib::FLAC::File tags(changed.c_str());
        tags.tag()->setTitle("Retitled");
        ASSERT_TRUE(tags.save());
    }
    fs::last_write_time(changed, fs::last_write_time(changed) + std::chrono::seconds(2));
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE title='Retitled'"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE album_id IS NOT NULL"), kFolders * kFiles);
    // A few steps for the changed track and the album rebuilds, never one per track of the root.
    EXPECT_LT(count(store, "SELECT value FROM library_revision WHERE id=1") - revision, kFiles);
}

TEST_F(LibraryScannerTest, AacMetadataUsesContainerTimingWithoutDecodingAudio) {
    using namespace camel::audio;
    using namespace camel::audio::testaudio;
    const auto path = dir / "music" / "album" / "track.m4a";
    writeAac(path, makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2, false);
    const auto expected = probeAacInfo(path);
    EXPECT_EQ(expected.totalFrames, openDecoder(DecoderKind::Symphonia, path)->info().totalFrames);
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const std::string marker = "mdat";
    const auto found = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    ASSERT_NE(found, bytes.end());
    const auto offset = size_t(found - bytes.begin());
    ASSERT_GE(offset, 4u);
    const uint32_t size = (uint32_t(uint8_t(bytes[offset-4])) << 24) | (uint32_t(uint8_t(bytes[offset-3])) << 16) |
                          (uint32_t(uint8_t(bytes[offset-2])) << 8) | uint8_t(bytes[offset-1]);
    ASSERT_GE(size, 8u);
    ASSERT_LE(offset - 4 + size, bytes.size());
    std::fill(bytes.begin() + offset + 4, bytes.begin() + offset - 4 + size, 0);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), std::streamsize(bytes.size()));
    output.close();
    EXPECT_EQ(probeAacInfo(path), expected);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    EXPECT_EQ(count(store, "SELECT total_frames FROM tracks"), int64_t(expected.totalFrames));
}

TEST_F(LibraryScannerTest, CueSheetsAreIgnoredAndEachFileIsOneTrack) {
    const auto source = audio();
    std::ofstream(source.parent_path() / "disc.cue") << "FILE \"one.flac\" WAVE\n"
                                                       "  TRACK 01 AUDIO\n"
                                                       "    INDEX 01 00:00:00\n"
                                                       "  TRACK 02 AUDIO\n"
                                                       "    INDEX 01 00:01:00\n";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto report = scanner.scan(scanner.addRoot(dir / "music"));
    ASSERT_TRUE(report.complete);
    EXPECT_TRUE(report.diagnostics.empty());
    EXPECT_EQ(count(store), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks"), 1);
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
        if (path.filename() == "slow.wav") {
            started.set_value();
            gate.wait();
        }
        return opener(path, format, bits);
    });
    fs::create_directory(dir / "music" / "00-slow");
    using namespace camel::audio::testaudio;
    writeWav(dir / "music" / "00-slow" / "slow.wav", makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
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

TEST_F(LibraryScannerTest, SlowFileDoesNotBlockPublishingOtherFilesInTheSameDirectory) {
    using namespace camel::audio;
    using namespace camel::audio::testaudio;
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::promise<void> started, release;
    const auto gate = release.get_future().share();
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        if (path.filename() == "slow.wav") { started.set_value(); gate.wait(); }
        return original(path, format, bits);
    });
    const auto folder = dir / "music" / "album";
    writeWav(folder / "slow.wav", makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
    for (int i = 0; i < 600; ++i) std::ofstream(folder / (std::to_string(i) + ".lrc")) << "lyrics";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root); });
    bool released = false;
    auto unblock = wil::scope_exit([&] { if (!released) release.set_value(); });
    EXPECT_EQ(started.get_future().wait_for(std::chrono::seconds(8)), std::future_status::ready);
    EXPECT_TRUE(waitFor([&] { return count(store) >= 500; }));
    release.set_value();
    released = true;
    EXPECT_TRUE(job.get().complete);
    EXPECT_EQ(count(store), 601);
}

TEST_F(LibraryScannerTest, FingerprintsDeferDuringAScanAndResumeAfterItFinishes) {
    using namespace camel::audio;
    using namespace camel::audio::testaudio;
    const auto folder = dir / "music" / "album";
    std::ofstream(folder / "words.lrc") << "lyrics";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::promise<void> started, release;
    const auto gate = release.get_future().share();
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        started.set_value();
        gate.wait();
        return original(path, format, bits);
    });
    writeWav(folder / "slow.wav", makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
    auto job = std::async(std::launch::async, [&] { return scanner.scan(root); });
    bool released = false;
    auto unblock = wil::scope_exit([&] { if (!released) release.set_value(); });
    EXPECT_EQ(started.get_future().wait_for(std::chrono::seconds(8)), std::future_status::ready);
    const auto deferred = scanner.fingerprint(root);
    EXPECT_FALSE(deferred.complete);
    EXPECT_EQ(deferred.hashed, 0);
    release.set_value();
    released = true;
    ASSERT_TRUE(job.get().complete);
    EXPECT_TRUE(scanner.fingerprint(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 2);
}

TEST_F(LibraryScannerTest, UnchangedPendingFilesReuseTheirRecordedFingerprint) {
    std::ofstream(dir / "music" / "album" / "words.lrc") << "lyrics";
    bool playing = false;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, [&] { return playing; });
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
    store.write([](sqlite::Database& db) { db.exec("UPDATE files SET identity_state=1"); }).get();
    playing = true;
    const auto report = scanner.scan(root);
    EXPECT_TRUE(report.complete);
    EXPECT_FALSE(report.deferred);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL AND identity_state=0"), 1);
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
    const auto renameWhenReleased = [&](const fs::path& from, const fs::path& to) {
        return waitFor([&] {
            std::error_code error;
            fs::rename(from, to, error);
            return !error;
        });
    };
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
    ASSERT_TRUE(renameWhenReleased(dir / "music", dir / "offline"));
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots WHERE removed IS NULL") == 0; }));
    EXPECT_EQ(count(store), 1);
    ASSERT_TRUE(renameWhenReleased(dir / "offline", dir / "music"));
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

TEST_F(LibraryScannerTest, AReachableRootIsOnlineBeforeItsScanFinishes) {
    using namespace camel::audio;
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::atomic<bool> release = false;
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return original(path, format, bits);
    });
    fs::create_directories(dir / "music" / "album");
    testaudio::writeWav(dir / "music" / "album" / "a.wav", testaudio::makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    store.write([](sqlite::Database& db) { db.exec("UPDATE roots SET online=0"); }).get();
    LibraryMonitor::Options options;
    options.offlineInterval = std::chrono::milliseconds(50);
    LibraryMonitor monitor(store, options);
    auto finish = wil::scope_exit([&] { release = true; }); // before the monitor joins its blocked scan
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT online FROM roots WHERE removed IS NULL") == 1; }));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL"), 0);
}

TEST_F(LibraryScannerTest, AlbumsAppearWithEachBatchOfAFirstScan) {
    using namespace camel::audio;
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::atomic<bool> release = false;
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        while (path.filename() == L"late.wav" && !release) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return original(path, format, bits);
    });
    const auto signal = testaudio::makeSignal({"short", 44100, 16, 2, 0.01});
    fs::create_directories(dir / "music" / "first");
    fs::create_directories(dir / "music" / "late");
    for (int i = 0; i < 500; ++i) testaudio::writeWav(dir / "music" / "first" / (std::to_string(i) + ".wav"), signal, 44100, 16, 2);
    testaudio::writeWav(dir / "music" / "late" / "late.wav", signal, 44100, 16, 2);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    auto scan = std::async(std::launch::async, [&] { return scanner.scan(root); });
    auto finish = wil::scope_exit([&] { release = true; }); // before the scan future joins
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM albums") == 1; }));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE album_id IS NOT NULL"), 500);
    const auto progress = store.scanProgress();
    EXPECT_EQ(progress.scans, 1);
    EXPECT_EQ(progress.found, 501);
    EXPECT_EQ(progress.checked, 500);
    EXPECT_EQ(progress.finishing, 0);
    release = true;
    EXPECT_TRUE(scan.get().complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums"), 2);
    EXPECT_EQ(store.scanProgress().scans, 0);
    EXPECT_EQ(store.scanProgress().found, 0);
}

TEST_F(LibraryScannerTest, AnotherRootIsAddedAndScannedWhileALongScanRuns) {
    using namespace camel::audio;
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::atomic<bool> release = false;
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        while (path.filename() == L"late.wav" && !release) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return original(path, format, bits);
    });
    const auto signal = testaudio::makeSignal({"short", 44100, 16, 2, 0.01});
    fs::create_directories(dir / "music" / "late");
    fs::create_directories(dir / "other" / "album");
    testaudio::writeWav(dir / "music" / "late" / "late.wav", signal, 44100, 16, 2);
    testaudio::writeWav(dir / "other" / "album" / "b.wav", signal, 44100, 16, 2);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto first = scanner.addRoot(dir / "music");
    std::optional<std::future<int64_t>> add;
    auto slow = std::async(std::launch::async, [&] { return scanner.scan(first); });
    auto finish = wil::scope_exit([&] { release = true; }); // before the futures join
    ASSERT_TRUE(waitFor([&] { return store.scanProgress().found == 1; }));
    add = std::async(std::launch::async, [&] { return scanner.addRoot(dir / "other"); });
    ASSERT_EQ(add->wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const auto second = add->get();
    EXPECT_TRUE(scanner.scan(second).complete); // while the first root's scan still waits
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums"), 1);
    release = true;
    EXPECT_TRUE(slow.get().complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums"), 2);
}

TEST_F(LibraryScannerTest, AnUnchangedTrackWithoutAnAlbumIsGroupedByTheNextScan) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET album_id=NULL; DELETE FROM albums;"); }).get();
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM albums"), 1);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE album_id IS NOT NULL"), 1);
}

TEST_F(LibraryScannerTest, PlaybackReducesDirectoryWorkersAndPauseRestoresConcurrency) {
    using namespace camel::audio;
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k"));
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    std::atomic<int> active = 0, maximum = 0;
    decoderRegistry().setOpener("sndfile", [&](const fs::path& path, std::string_view format, uint32_t bits) {
        const auto workers = ++active;
        auto done = wil::scope_exit([&] { --active; });
        int previous = maximum.load();
        while (workers > previous && !maximum.compare_exchange_weak(previous, workers)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        return original(path, format, bits);
    });
    bool playing = true;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, [&] { return playing; });
    const auto root = scanner.addRoot(dir / "music");
    for (int pass = 0; pass < 2; ++pass) {
        for (int album = 0; album < 4; ++album) {
            const auto folder = dir / "music" / std::to_string(album);
            fs::create_directories(folder);
            using namespace camel::audio::testaudio;
            writeWav(folder / (std::to_string(pass) + ".wav"), makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
        }
        maximum = 0;
        ASSERT_TRUE(scanner.scan(root).complete);
        if (playing) EXPECT_EQ(maximum.load(), 1);
        else EXPECT_GT(maximum.load(), 1);
        EXPECT_LE(maximum.load(), 4);
        playing = false;
    }
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

TEST_F(LibraryScannerTest, AFolderScanReconcilesOnlyThatSubtree) {
    audio();
    fs::create_directories(dir / "music" / "other");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "other" / "x.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto scanned = count(store, "SELECT last_scan FROM roots");
    auto row = store.openReader().prepare("SELECT path FROM roots");
    ASSERT_TRUE(row.step());
    const fs::path stored(camel::toWide(row.text(0)));

    audio("two.flac");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "other" / "y.flac");
    fs::remove(dir / "music" / "other" / "x.flac");
    ASSERT_TRUE(scanner.scan(root, {}, {stored / "album"}).complete);
    EXPECT_EQ(count(store), 3); // one, two, and x still listed: "other" was not scanned
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE path LIKE '%y.flac'"), 0);
    EXPECT_EQ(count(store, "SELECT last_scan FROM roots"), scanned);

    ASSERT_TRUE(scanner.scan(root, {}, {stored / "other"}).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE available=1 AND path LIKE '%x.flac'"), 0);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM files WHERE available=1 AND path LIKE '%y.flac'"), 1);

    fs::remove_all(dir / "music" / "album");
    ASSERT_TRUE(scanner.scan(root, {}, {stored / "album"}).complete);
    EXPECT_EQ(count(store), 1);
}

TEST_F(LibraryScannerTest, ANotifiedFolderIsScannedWithoutTheWholeRoot) {
    audio();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(0);
    options.offlineInterval = std::chrono::milliseconds(100);
    options.coalesce = std::chrono::milliseconds(100);
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return count(store, "SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL") == 1; }));
    const auto scanned = count(store, "SELECT last_scan FROM roots");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // a full scan would record a later time
    fs::create_directories(dir / "music" / "new album");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "new album" / "a.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 2; }));
    EXPECT_EQ(count(store, "SELECT last_scan FROM roots"), scanned);
    fs::remove(dir / "music" / "new album" / "a.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    EXPECT_EQ(count(store, "SELECT last_scan FROM roots"), scanned);
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "new album" / "b.flac");
    ASSERT_TRUE(waitFor([&] { return count(store) == 2; }));
    fs::remove_all(dir / "music" / "new album");
    ASSERT_TRUE(waitFor([&] { return count(store) == 1; }));
    EXPECT_EQ(count(store, "SELECT last_scan FROM roots"), scanned);
}
