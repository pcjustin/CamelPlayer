// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/synchronizedlyricsframe.h>
#include <taglib/tpropertymap.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "LameFixture.h"
#include "Server/Library/Lyrics.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

std::vector<std::string> texts(const Lyrics& lyrics) {
    std::vector<std::string> out;
    out.reserve(lyrics.lines.size());
    for (const auto& line : lyrics.lines) out.push_back(line.text);
    return out;
}

std::vector<uint32_t> times(const Lyrics& lyrics) {
    std::vector<uint32_t> out;
    out.reserve(lyrics.lines.size());
    for (const auto& line : lyrics.lines) out.push_back(line.ms.value_or(UINT32_MAX));
    return out;
}

std::filesystem::path freshDir(const char* name) {
    const auto dir = testutil::tempDir() / "lyrics" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path flacIn(const std::filesystem::path& dir) {
    const auto file = dir / "track.flac";
    std::filesystem::copy_file(testutil::generatedFile({"lyrics-source", 44100, 16, 2, 0.2}), file);
    return file;
}

void tagFlac(const std::filesystem::path& file, const char* key, const std::string& value) {
    TagLib::FLAC::File tags(file.c_str());
    auto properties = tags.properties();
    properties.replace(key, TagLib::StringList(TagLib::String(value, TagLib::String::UTF8)));
    tags.setProperties(properties);
    ASSERT_TRUE(tags.save());
}

void writeLrc(const std::filesystem::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary) << text;
}

constexpr const char* kTranslated = "[by:someone]\n[00:00.80]Living in my own world\n[00:00.80]活在自己的世界裡\n"
                                    "[00:04.96]Didn't understand\n[00:04.96]卻殊不知\n";

} // namespace

TEST(LyricsTest, StampsMakeSyncedLinesInTimeOrder) {
    const auto lyrics = parseLrc("[00:12.50]second\n[00:01.2]first\n[01:02.345]third\n");
    EXPECT_TRUE(lyrics.synced);
    EXPECT_EQ(texts(lyrics), (std::vector<std::string>{"first", "second", "third"}));
    EXPECT_EQ(times(lyrics), (std::vector<uint32_t>{1200, 12500, 62345}));
}

TEST(LyricsTest, ALineWithSeveralStampsIsSungAtEach) {
    const auto lyrics = parseLrc("[00:10.00][00:30.00]chorus\n[00:20.00]verse\n");
    EXPECT_EQ(texts(lyrics), (std::vector<std::string>{"chorus", "verse", "chorus"}));
    EXPECT_EQ(times(lyrics), (std::vector<uint32_t>{10000, 20000, 30000}));
}

TEST(LyricsTest, LinesOfOneStampKeepTheirOrderAndIdTagsAreLeftOut) {
    const auto lyrics = parseLrc(kTranslated);
    EXPECT_EQ(texts(lyrics),
              (std::vector<std::string>{"Living in my own world", "活在自己的世界裡", "Didn't understand", "卻殊不知"}));
    EXPECT_EQ(times(lyrics), (std::vector<uint32_t>{800, 800, 4960, 4960}));
}

TEST(LyricsTest, TheOffsetMovesEveryLine) {
    EXPECT_EQ(times(parseLrc("[offset:+500]\n[00:02.00]a\n[00:00.20]b\n")), (std::vector<uint32_t>{0, 1500}));
    EXPECT_EQ(times(parseLrc("[offset:-250]\n[00:02.00]a\n")), (std::vector<uint32_t>{2250}));
}

TEST(LyricsTest, MalformedLinesAreSkippedAndTextWithoutStampsIsUnsynced) {
    const auto synced = parseLrc("[00:01.00]one\n[xx:yy]broken\nno stamp\n[00:61.00]bad seconds\n[00:02]two\n");
    EXPECT_EQ(texts(synced), (std::vector<std::string>{"one", "two"}));
    const auto plain = parseLrc("\xEF\xBB\xBFLine one\r\n\r\nLine two\r\n");
    EXPECT_FALSE(plain.synced);
    EXPECT_EQ(texts(plain), (std::vector<std::string>{"Line one", "Line two"}));
    EXPECT_TRUE(parseLrc("").lines.empty());
}

TEST(LyricsTest, TheLineAtAPositionIsTheFirstOfItsStamp) {
    const auto lyrics = parseLrc(kTranslated);
    EXPECT_FALSE(lyricLineAt(lyrics, 500).has_value());
    EXPECT_EQ(lyricLineAt(lyrics, 800), 0U);
    EXPECT_EQ(lyricLineAt(lyrics, 4000), 0U);
    EXPECT_EQ(lyricLineAt(lyrics, 4960), 2U);
    EXPECT_EQ(lyricLineAt(lyrics, 99000), 2U);
    EXPECT_FALSE(lyricLineAt(parseLrc("plain"), 1000).has_value());
}

TEST(LyricsTest, VorbisLyricsAndUnsyncedLyricsAreRead) {
    const auto dir = freshDir("vorbis");
    const auto file = flacIn(dir);
    tagFlac(file, "LYRICS", kTranslated);
    EXPECT_EQ(texts(readLyrics(file)).size(), 4U);
    const auto other = freshDir("vorbis-unsynced");
    const auto plain = flacIn(other);
    tagFlac(plain, "UNSYNCEDLYRICS", "Just words\nMore words");
    const auto lyrics = readLyrics(plain);
    EXPECT_FALSE(lyrics.synced);
    EXPECT_EQ(texts(lyrics), (std::vector<std::string>{"Just words", "More words"}));
}

TEST(LyricsTest, Id3UnsyncedAndSyncedFramesAreRead) {
    const auto dir = freshDir("id3");
    const auto file = dir / "track.mp3";
    testutil::writeMp3(file, std::vector<int32_t>(44100, 0), 44100, 16, 2, true);
    {
        TagLib::MPEG::File mp3(file.c_str());
        auto* uslt = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
        uslt->setText(TagLib::String("Words alone", TagLib::String::UTF8));
        mp3.ID3v2Tag(true)->addFrame(uslt);
        ASSERT_TRUE(mp3.save());
    }
    EXPECT_EQ(texts(readLyrics(file)), (std::vector<std::string>{"Words alone"}));
    {
        TagLib::MPEG::File mp3(file.c_str());
        auto* sylt = new TagLib::ID3v2::SynchronizedLyricsFrame(TagLib::String::UTF8);
        sylt->setTimestampFormat(TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds);
        sylt->setSynchedText(
            {{3000, TagLib::String("later", TagLib::String::UTF8)}, {1000, TagLib::String("sooner", TagLib::String::UTF8)}});
        mp3.ID3v2Tag(true)->addFrame(sylt);
        ASSERT_TRUE(mp3.save());
    }
    const auto lyrics = readLyrics(file);
    EXPECT_TRUE(lyrics.synced); // synced over unsynced
    EXPECT_EQ(texts(lyrics), (std::vector<std::string>{"sooner", "later"}));
    EXPECT_EQ(times(lyrics), (std::vector<uint32_t>{1000, 3000}));
}

TEST(LyricsTest, Mp4LyricsAreRead) {
    const auto dir = freshDir("mp4");
    const auto file = dir / "track.m4a";
    testutil::copyFixture("alac-short.m4a", file);
    {
        TagLib::MP4::File m4a(file.c_str());
        auto properties = m4a.properties();
        properties.replace("LYRICS", TagLib::StringList(TagLib::String("[00:01.00]in an atom", TagLib::String::UTF8)));
        m4a.setProperties(properties);
        ASSERT_TRUE(m4a.save());
    }
    EXPECT_EQ(texts(readLyrics(file)), (std::vector<std::string>{"in an atom"}));
}

TEST(LyricsTest, ALrcBesideTheFileWinsOverEmbeddedOfTheSameKind) {
    const auto dir = freshDir("choice");
    const auto file = flacIn(dir);
    tagFlac(file, "LYRICS", "[00:01.00]embedded");
    writeLrc(dir / "track.lrc", "\xEF\xBB\xBF[00:01.00]from the lrc\n");
    EXPECT_EQ(texts(readLyrics(file)), (std::vector<std::string>{"from the lrc"}));
    writeLrc(dir / "track.lrc", "plain words in the lrc\n");
    EXPECT_EQ(texts(readLyrics(file)), (std::vector<std::string>{"embedded"})); // synced over unsynced
    std::filesystem::remove(dir / "track.lrc");
    tagFlac(file, "LYRICS", "");
    EXPECT_TRUE(readLyrics(file).lines.empty());
    EXPECT_TRUE(readLyrics(dir / "missing.flac").lines.empty());
}
