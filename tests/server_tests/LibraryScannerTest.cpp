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
#include "Server/Library/LibraryText.h"
#include "Common/DecoderRegistry.h"
#include "Engine/decode/IDecoder.h"
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

void embedCueSheet(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    ASSERT_GE(bytes.size(), 8u);
    ASSERT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "fLaC");
    size_t audioOffset = 4;
    while (true) {
        ASSERT_LE(audioOffset + 4, bytes.size());
        const bool last = (bytes[audioOffset] & 0x80) != 0;
        const size_t length =
            (size_t(bytes[audioOffset + 1]) << 16) | (size_t(bytes[audioOffset + 2]) << 8) | bytes[audioOffset + 3];
        audioOffset += 4 + length;
        ASSERT_LE(audioOffset, bytes.size());
        if (last) {
            bytes[audioOffset - length - 4] &= 0x7f;
            break;
        }
    }
    std::vector<uint8_t> cue(396, 0);
    cue[395] = 3;
    const auto track = [&cue](uint64_t offset, uint8_t number, uint8_t indices) {
        for (int i = 7; i >= 0; --i) cue.push_back(uint8_t(offset >> (i * 8)));
        cue.push_back(number);
        cue.insert(cue.end(), 12, 0);
        cue.push_back(0);
        cue.insert(cue.end(), 13, 0);
        cue.push_back(indices);
    };
    const auto index = [&cue](uint8_t number) {
        cue.insert(cue.end(), 8, 0);
        cue.push_back(number);
        cue.insert(cue.end(), 3, 0);
    };
    track(0, 1, 1);
    index(1);
    track(44100, 2, 1);
    index(1);
    track(88200, 170, 0);
    std::vector<uint8_t> block{0x85, uint8_t(cue.size() >> 16), uint8_t(cue.size() >> 8), uint8_t(cue.size())};
    block.insert(block.end(), cue.begin(), cue.end());
    bytes.insert(bytes.begin() + ptrdiff_t(audioOffset), block.begin(), block.end());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

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
    EXPECT_EQ(report.diagnostics.size(), 3); // The malformed CUE is reported.
    EXPECT_EQ(count(store), 6);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE format='flac'"), 1);
}

TEST_F(LibraryScannerTest, ExternalCueBuildsVirtualRangesAndRestoresWholeFileWhenRemoved) {
    const auto source = audio();
    const auto cue = source.parent_path() / "disc.cue";
    std::ofstream(cue) << "TITLE \"Album from CUE\"\n"
                          "PERFORMER \"Cue artist\"\n"
                          "REM DATE 1998\n"
                          "FILE \"one.flac\" WAVE\n"
                          "  TRACK 01 AUDIO\n"
                          "    TITLE \"First song\"\n"
                          "    INDEX 01 00:00:10\n"
                          "  TRACK 02 AUDIO\n"
                          "    TITLE \"Second song\"\n"
                          "    INDEX 00 00:01:00\n"
                          "    INDEX 01 00:01:10\n"
                          "    PREGAP 00:00:05\n"
                          "    POSTGAP 00:00:03\n";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    const auto first = scanner.scan(root);
    ASSERT_TRUE(first.complete);
    EXPECT_TRUE(first.diagnostics.empty());
    auto db = store.openReader();
    auto rows = db.prepare("SELECT cue_index,title,album,artist,year,start_frame,end_frame,cue_file_id,"
                           "cue_pregap_75ths,cue_postgap_75ths FROM tracks WHERE cue_index<>0 ORDER BY cue_index");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.integer(0), -1);
    EXPECT_EQ(rows.integer(5), 0);
    EXPECT_EQ(rows.integer(6), 5880);
    EXPECT_EQ(rows.integer(7), count(store, "SELECT id FROM files WHERE kind='cue'"));
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.integer(0), 1);
    EXPECT_EQ(rows.text(1), "First song");
    EXPECT_EQ(rows.text(2), "Album from CUE");
    EXPECT_EQ(rows.text(3), "Cue artist");
    EXPECT_EQ(rows.integer(4), 1998);
    EXPECT_EQ(rows.integer(5), 5880);
    EXPECT_EQ(rows.integer(6), 49980); // The stored INDEX 00 pregap belongs to track 1.
    const auto firstTrack = count(store, "SELECT id FROM tracks WHERE cue_index=1");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.integer(0), 2);
    EXPECT_EQ(rows.integer(5), 49980);
    EXPECT_EQ(rows.integer(6), 88200);
    EXPECT_EQ(rows.integer(8), 5);
    EXPECT_EQ(rows.integer(9), 3);
    EXPECT_FALSE(rows.step());
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index=0"), 1); // retained as the metadata source
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM library_tracks"), 2);
    store
        .write([=](sqlite::Database& w) {
            w.prepare("INSERT INTO stats(file_id,cue_index,rating) SELECT file_id,1,5 FROM tracks WHERE cue_index=0").run();
        })
        .get();
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT id FROM tracks WHERE cue_index=1"), firstTrack);
    EXPECT_EQ(count(store, "SELECT rating FROM stats WHERE cue_index=1"), 5);
    fs::remove(cue);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index<>0"), 0);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index=0"), 1);
}

TEST_F(LibraryScannerTest, ValidExternalCueOverridesEmbeddedSheetAndRemovalRestoresIt) {
    const auto source = audio();
    embedCueSheet(source);
    const auto cue = source.parent_path() / "disc.cue";
    std::ofstream(cue) << "FILE \"one.flac\" WAVE\n"
                          "  TRACK 01 AUDIO\n"
                          "    TITLE \"External one\"\n"
                          "    INDEX 01 00:00:10\n"
                          "  TRACK 02 AUDIO\n"
                          "    TITLE \"External two\"\n"
                          "    INDEX 01 00:01:00\n";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    {
        auto db = store.openReader();
        auto track = db.prepare("SELECT title,start_frame,cue_file_id FROM tracks WHERE cue_index=1");
        ASSERT_TRUE(track.step());
        EXPECT_EQ(track.text(0), "External one");
        EXPECT_EQ(track.integer(1), 5880);
        EXPECT_NE(track.integer(2), 0);
    }
    fs::remove(cue);
    ASSERT_TRUE(scanner.scan(root).complete);
    auto db = store.openReader();
    auto track = db.prepare("SELECT title,start_frame,end_frame,cue_file_id FROM tracks WHERE cue_index=1");
    ASSERT_TRUE(track.step());
    EXPECT_EQ(track.text(0), "Track 1");
    EXPECT_EQ(track.integer(1), 0);
    EXPECT_EQ(track.integer(2), 44100);
    EXPECT_TRUE(track.isNull(3));
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index=2"), 1);
}

TEST_F(LibraryScannerTest, MultiFileCueUsesFileOrderAndRejectsEscapes) {
    audio("first.flac");
    audio("second.flac");
    const auto cue = dir / "music" / "album" / "disc.cue";
    std::ofstream(cue) << "FILE \"first.flac\" WAVE\n"
                          "  TRACK 01 AUDIO\n"
                          "    TITLE \"One\"\n"
                          "    INDEX 01 00:00:00\n"
                          "FILE \"second.flac\" WAVE\n"
                          "  TRACK 02 AUDIO\n"
                          "    TITLE \"Two\"\n"
                          "    INDEX 01 00:00:00\n";
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index>0"), 2);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index=0"), 2);
    {
        auto db = store.openReader();
        auto rows = db.prepare("SELECT t.cue_index,f.path,t.start_frame,t.end_frame FROM tracks t JOIN files f ON f.id=t.file_id "
                               "WHERE t.cue_index>0 ORDER BY t.cue_index");
        ASSERT_TRUE(rows.step());
        EXPECT_EQ(rows.integer(0), 1);
        EXPECT_NE(rows.text(1).find("first.flac"), std::string::npos);
        EXPECT_EQ(rows.integer(2), 0);
        EXPECT_EQ(rows.integer(3), 88200);
        ASSERT_TRUE(rows.step());
        EXPECT_EQ(rows.integer(0), 2);
        EXPECT_NE(rows.text(1).find("second.flac"), std::string::npos);
        EXPECT_EQ(rows.integer(2), 0);
        EXPECT_EQ(rows.integer(3), 88200);
    }
    std::ofstream(cue) << "FILE \"..\\..\\outside.flac\" WAVE\n"
                          "  TRACK 01 AUDIO\n"
                          "    INDEX 01 00:00:00\n";
    fs::last_write_time(cue, fs::last_write_time(cue) + std::chrono::seconds(2));
    const auto invalid = scanner.scan(root);
    ASSERT_TRUE(invalid.complete);
    EXPECT_FALSE(invalid.diagnostics.empty());
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index=0"), 2);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index>0"), 0);
}

TEST_F(LibraryScannerTest, LegacyCueEncodingCanBeSetAndCleared) {
    const auto source = audio();
    const auto cue = source.parent_path() / "legacy.cue";
    const std::wstring title = L"歌名";
    const int bytesNeeded = WideCharToMultiByte(950, 0, title.data(), int(title.size()), nullptr, 0, nullptr, nullptr);
    ASSERT_GT(bytesNeeded, 0);
    std::string encoded(size_t(bytesNeeded), '\0');
    ASSERT_EQ(WideCharToMultiByte(950, 0, title.data(), int(title.size()), encoded.data(), bytesNeeded, nullptr, nullptr),
              bytesNeeded);
    std::ofstream output(cue, std::ios::binary);
    output << "FILE \"one.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"";
    output.write(encoded.data(), std::streamsize(encoded.size()));
    output << "\"\n    INDEX 01 00:00:00\n";
    output.close();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(count(store, "SELECT COUNT(*) FROM tracks WHERE cue_index>0"), 0);
    scanner.setCueEncoding(cue, 950);
    ASSERT_TRUE(scanner.scan(root).complete);
    auto db = store.openReader();
    auto row = db.prepare("SELECT title FROM tracks WHERE cue_index=1");
    ASSERT_TRUE(row.step());
    EXPECT_EQ(row.text(0), "歌名");
    const auto renamedCue = cue.parent_path() / "renamed.cue";
    fs::rename(cue, renamedCue);
    ASSERT_TRUE(scanner.scan(root).complete);
    std::ofstream(renamedCue) << "FILE \"one.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"UTF-8 優先\"\n"
                                 "    INDEX 01 00:00:00\n";
    fs::last_write_time(renamedCue, fs::last_write_time(renamedCue) + std::chrono::seconds(2));
    ASSERT_TRUE(scanner.scan(root).complete);
    auto utf8db = store.openReader();
    auto utf8row = utf8db.prepare("SELECT title FROM tracks WHERE cue_index=1");
    ASSERT_TRUE(utf8row.step());
    EXPECT_EQ(utf8row.text(0), "UTF-8 優先");
    scanner.setCueEncoding(renamedCue, std::nullopt);
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM app_state WHERE key LIKE 'cue_encoding:%'"), 0);
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
            fs::copy_file(testutil::testFile("16-44.1k"), folder / (std::to_string(pass) + ".flac"));
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
