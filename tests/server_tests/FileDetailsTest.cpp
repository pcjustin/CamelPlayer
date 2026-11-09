// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <taglib/aifffile.h>
#include <taglib/apetag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/commentsframe.h>
#include <taglib/dsdiffdiintag.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/infotag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/privateframe.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tpropertymap.h>
#include <taglib/uniquefileidentifierframe.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/wavfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "DsdFixture.h"
#include "LameFixture.h"
#include "Server/Library/FileDetails.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryStore.h"
#include "Common/util/Text.h"
#include "TestUtil.h"
#include "Tools/testaudio/TestAudio.h"

using namespace camel::server;
namespace fs = std::filesystem;

namespace {

TagLib::String u8(const std::string& text) {
    return {text, TagLib::String::UTF8};
}

class FileDetailsTest : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        dir = testutil::tempDir() / "file-details" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::remove_all(dir);
        fs::create_directories(dir / "music");
    }
    fs::path flac() const {
        const auto file = dir / "music" / "track.flac";
        fs::copy_file(testutil::testFile("16-44.1k"), file);
        return file;
    }
    fs::path mp3() const {
        const auto file = dir / "music" / "track.mp3";
        testutil::writeMp3(file, std::vector<int32_t>(size_t{2} * 44100, 0), 44100, 16, 2, true);
        return file;
    }
    static FileDetails read(const fs::path& file, sqlite::Database* db = nullptr) {
        return readFileDetails(camel::toUtf8(file.wstring()), db);
    }
};

const FileTagSection* sectionOf(const FileDetails& details, const std::string& format) {
    const auto found = std::ranges::find(details.sections, format, &FileTagSection::format);
    return found == details.sections.end() ? nullptr : &*found;
}

const FileTag* tagOf(const FileTagSection* section, const std::string& key) {
    if (!section) return nullptr;
    const auto found = std::ranges::find(section->tags, key, &FileTag::key);
    return found == section->tags.end() ? nullptr : &*found;
}

std::vector<std::string> formats(const FileDetails& details) {
    std::vector<std::string> out;
    out.reserve(details.sections.size());
    for (const auto& section : details.sections) out.push_back(section.format);
    return out;
}

using Values = std::vector<std::string>;

} // namespace

TEST_F(FileDetailsTest, VorbisCommentsKeepEveryKeyAndValueButPicturesAndLyrics) {
    const auto file = flac();
    {
        TagLib::FLAC::File tags(file.c_str());
        auto* comment = tags.xiphComment(true);
        comment->addField("ARTIST", u8("Ann"));
        comment->addField("ARTIST", u8("Bob"), false);
        comment->addField("ISRC", "USRC17607839");
        comment->addField("MY_OWN_KEY", u8("custom value"));
        comment->addField("LYRICS", "[00:01.00]words");
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/jpeg");
        picture->setData(TagLib::ByteVector(64, 'x'));
        tags.addPicture(picture);
        ASSERT_TRUE(tags.save());
    }
    const auto details = read(file);
    EXPECT_EQ(details.problem, FileDetails::Problem::None);
    EXPECT_EQ(formats(details), (Values{"Vorbis comment"}));
    const auto* vorbis = sectionOf(details, "Vorbis comment");
    ASSERT_NE(tagOf(vorbis, "ARTIST"), nullptr);
    EXPECT_EQ(tagOf(vorbis, "ARTIST")->values, (Values{"Ann", "Bob"}));
    EXPECT_EQ(tagOf(vorbis, "ARTIST")->field, "ARTIST");
    EXPECT_EQ(tagOf(vorbis, "ISRC")->values, (Values{"USRC17607839"}));
    EXPECT_EQ(tagOf(vorbis, "MY_OWN_KEY")->values, (Values{"custom value"}));
    EXPECT_EQ(tagOf(vorbis, "LYRICS"), nullptr);
    EXPECT_EQ(tagOf(vorbis, "METADATA_BLOCK_PICTURE"), nullptr);
    EXPECT_FALSE(tagOf(vorbis, "ARTIST")->library.has_value()); // nothing to compare with
}

TEST_F(FileDetailsTest, AnMp3ShowsId3v2ApeAndId3v1EachWithItsOwnKeys) {
    const auto file = mp3();
    {
        TagLib::MPEG::File tags(file.c_str());
        auto* id3 = tags.ID3v2Tag(true);
        id3->setTitle(u8("Title v2"));
        id3->setArtist(u8("Artist"));
        auto* user = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
        user->setDescription("MusicBrainz Album Id");
        user->setText("0f3c1b6a");
        id3->addFrame(user);
        auto* own = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);
        own->setDescription("my note");
        own->setText(u8("first"));
        id3->addFrame(own);
        auto* comment = new TagLib::ID3v2::CommentsFrame(TagLib::String::UTF8);
        comment->setText(u8("A comment"));
        id3->addFrame(comment);
        auto* picture = new TagLib::ID3v2::AttachedPictureFrame;
        picture->setPicture(TagLib::ByteVector(32, 'p'));
        id3->addFrame(picture);
        auto* lyrics = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
        lyrics->setText(u8("words"));
        id3->addFrame(lyrics);
        auto* priv = new TagLib::ID3v2::PrivateFrame;
        priv->setOwner("owner");
        priv->setData(TagLib::ByteVector(8, '\1'));
        id3->addFrame(priv);
        id3->addFrame(new TagLib::ID3v2::UniqueFileIdentifierFrame("http://musicbrainz.org", "1234"));
        auto* ape = tags.APETag(true);
        ape->setArtist(u8("APE artist"));
        ape->setData("Cover Art (Front)", TagLib::ByteVector(16, 'c'));
        ape->setData("Binary", TagLib::ByteVector(4, 'b'));
        tags.ID3v1Tag(true)->setTitle("Title v1");
        ASSERT_TRUE(tags.save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v4));
    }
    const auto details = read(file);
    EXPECT_EQ(formats(details), (Values{"ID3v2.4", "APEv2", "ID3v1"}));
    const auto* id3 = sectionOf(details, "ID3v2.4");
    EXPECT_EQ(tagOf(id3, "TIT2")->values, (Values{"Title v2"}));
    EXPECT_EQ(tagOf(id3, "TIT2")->field, "TITLE");
    EXPECT_EQ(tagOf(id3, "TPE1")->field, "ARTIST");
    EXPECT_EQ(tagOf(id3, "TXXX:MusicBrainz Album Id")->field, "MUSICBRAINZ_ALBUMID");
    EXPECT_EQ(tagOf(id3, "TXXX:my note")->values, (Values{"first"}));
    EXPECT_EQ(tagOf(id3, "COMM")->field, "COMMENT");
    for (const char* left : {"APIC", "USLT", "PRIV", "UFID"}) EXPECT_EQ(tagOf(id3, left), nullptr) << left;
    const auto* ape = sectionOf(details, "APEv2");
    ASSERT_NE(tagOf(ape, "ARTIST"), nullptr);
    EXPECT_EQ(tagOf(ape, "ARTIST")->values, (Values{"APE artist"}));
    EXPECT_EQ(ape->tags.size(), 1U); // binary items are left out
    const auto* v1 = sectionOf(details, "ID3v1");
    EXPECT_EQ(tagOf(v1, "TITLE")->values, (Values{"Title v1"}));
}

TEST_F(FileDetailsTest, AnId3v23TagIsNamedWithItsVersion) {
    const auto file = mp3();
    {
        TagLib::MPEG::File tags(file.c_str());
        tags.ID3v2Tag(true)->setTitle("Old version");
        ASSERT_TRUE(tags.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, TagLib::ID3v2::v3));
    }
    EXPECT_EQ(formats(read(file)), (Values{"ID3v2.3"}));
}

TEST_F(FileDetailsTest, Mp4AtomsShowTheirNamesAndLeaveOutCoversAndLyrics) {
    const auto file = dir / "music" / "track.m4a";
    testutil::copyFixture("alac-short.m4a", file);
    {
        TagLib::MP4::File tags(file.c_str());
        auto* tag = tags.tag();
        tag->setArtist(u8("Atom artist"));
        tag->setItem("----:com.apple.iTunes:ISRC", TagLib::StringList("USRC17607839"));
        tag->setItem("trkn", TagLib::MP4::Item(3, 12));
        tag->setItem("\xA9lyr", TagLib::StringList("words"));
        tag->setItem("covr", TagLib::MP4::CoverArtList().append(
                                 TagLib::MP4::CoverArt(TagLib::MP4::CoverArt::JPEG, TagLib::ByteVector(16, 'c'))));
        ASSERT_TRUE(tags.save());
    }
    const auto details = read(file);
    const auto* mp4 = sectionOf(details, "MP4");
    ASSERT_NE(mp4, nullptr);
    ASSERT_NE(tagOf(mp4, "\xC2\xA9"
                         "ART"),
              nullptr);
    EXPECT_EQ(tagOf(mp4, "\xC2\xA9"
                         "ART")
                  ->field,
              "ARTIST");
    EXPECT_EQ(tagOf(mp4, "----:com.apple.iTunes:ISRC")->field, "ISRC");
    EXPECT_EQ(tagOf(mp4, "trkn")->values, (Values{"3/12"}));
    EXPECT_EQ(tagOf(mp4, "trkn")->field, "TRACKNUMBER");
    EXPECT_EQ(tagOf(mp4, "\xC2\xA9"
                         "lyr"),
              nullptr);
    EXPECT_EQ(tagOf(mp4, "covr"), nullptr);
    EXPECT_EQ(details.format, "ALAC");
}

TEST_F(FileDetailsTest, AWavShowsRiffInfoAndItsId3Chunk) {
    const auto file = dir / "music" / "track.wav";
    camel::audio::testaudio::writeWav(file, std::vector<int32_t>(size_t{2} * 4410, 0), 44100, 16, 2);
    {
        TagLib::RIFF::WAV::File tags(file.c_str());
        tags.InfoTag()->setFieldText("INAM", u8("Info title"));
        tags.InfoTag()->setFieldText("ISFT", "Some encoder");
        tags.ID3v2Tag()->setTitle(u8("Chunk title"));
        ASSERT_TRUE(tags.save(TagLib::RIFF::WAV::File::AllTags));
    }
    const auto details = read(file);
    EXPECT_EQ(formats(details), (Values{"RIFF INFO", "ID3v2.4"}));
    const auto* info = sectionOf(details, "RIFF INFO");
    EXPECT_EQ(tagOf(info, "INAM")->field, "TITLE");
    EXPECT_EQ(tagOf(info, "INAM")->values, (Values{"Info title"}));
    EXPECT_NE(tagOf(info, "ISFT"), nullptr);
    EXPECT_EQ(tagOf(sectionOf(details, "ID3v2.4"), "TIT2")->values, (Values{"Chunk title"}));
    EXPECT_EQ(details.sampleRate, 44100U);
    EXPECT_EQ(details.frames, 4410U);
    EXPECT_EQ(details.lengthMs, 100U);
}

TEST_F(FileDetailsTest, AiffAndDsfCarryAnId3Tag) {
    const auto aiff = dir / "music" / "track.aiff";
    camel::audio::testaudio::writeAiff(aiff, std::vector<int32_t>(size_t{2} * 4410, 0), 44100, 24, 2);
    {
        TagLib::RIFF::AIFF::File tags(aiff.c_str());
        tags.tag()->setTitle(u8("AIFF title"));
        ASSERT_TRUE(tags.save());
    }
    const auto aiffDetails = read(aiff);
    EXPECT_EQ(tagOf(sectionOf(aiffDetails, "ID3v2.4"), "TIT2")->values, (Values{"AIFF title"}));
    EXPECT_EQ(aiffDetails.bitsPerSample, 24U);
    const auto dsf = dir / "music" / "track.dsf";
    dsdfixture::writeFile(dsf, dsdfixture::dsf(dsdfixture::rawDsd(size_t{4096} * 4, 2), {}));
    {
        TagLib::DSF::File tags(dsf.c_str());
        tags.tag()->setTitle(u8("DSF title"));
        ASSERT_TRUE(tags.save());
    }
    const auto dsfDetails = read(dsf);
    ASSERT_EQ(dsfDetails.sections.size(), 1U);
    EXPECT_EQ(tagOf(&dsfDetails.sections.front(), "TIT2")->values, (Values{"DSF title"}));
    EXPECT_EQ(dsfDetails.sampleRate, dsdfixture::kDsd64);
    EXPECT_EQ(dsfDetails.format, "DSD");
}

TEST_F(FileDetailsTest, DsdiffShowsItsDiinChunkAndId3Tag) {
    const auto file = dir / "music" / "track.dff";
    dsdfixture::writeFile(file, dsdfixture::dff(dsdfixture::rawDsd(4096, 2), 2, dsdfixture::kDsd64, false));
    {
        TagLib::DSDIFF::File tags(file.c_str());
        tags.DIINTag(true)->setTitle(u8("DIIN title"));
        tags.ID3v2Tag(true)->setArtist(u8("ID3 artist"));
        ASSERT_TRUE(tags.save());
    }
    const auto details = read(file);
    EXPECT_EQ(formats(details), (Values{"DIIN", "ID3v2.4"}));
    EXPECT_EQ(tagOf(sectionOf(details, "DIIN"), "DITI")->values, (Values{"DIIN title"}));
    EXPECT_EQ(tagOf(sectionOf(details, "ID3v2.4"), "TPE1")->values, (Values{"ID3 artist"}));
}

// Every format CamelPlayer plays opens, with its stream read; those not tested above with their tags.
TEST_F(FileDetailsTest, EveryPlayableFormatIsRead) {
    const auto music = dir / "music";
    testutil::copyFixture("aac-short.m4a", music / "aac.m4a");
    testutil::copyFixture("aac-short.aac", music / "adts.aac");
    testutil::copyFixture("dts-cd.wav", music / "dts.wav");
    camel::audio::testaudio::writeAdpcm(music / "adpcm.wav", std::vector<int32_t>(size_t{2} * 4410, 0), 44100, 2, false);
    camel::audio::testaudio::writeAiff(music / "compressed.aifc", std::vector<int32_t>(size_t{2} * 4410, 0), 44100, 16, 2);
    for (const auto& [name, format] : {std::pair{"aac.m4a", "AAC"},
                                       {"adts.aac", "AAC"},
                                       {"dts.wav", "DTS"},
                                       {"adpcm.wav", "ADPCM"},
                                       {"compressed.aifc", "AIFF"}}) {
        const auto details = read(music / name);
        EXPECT_EQ(details.problem, FileDetails::Problem::None) << name << ": " << details.reason;
        EXPECT_EQ(details.format, format) << name;
        EXPECT_EQ(details.sampleRate, 44100U) << name;
        EXPECT_EQ(details.bitsPerSample == 0, std::string_view(format) != "AIFF") << name; // lossy: no source depth
        if (std::string_view(name) == "adts.aac") continue; // raw ADTS declares no length; playback measures it
        EXPECT_GT(details.lengthMs, 0U) << name;
        EXPECT_GT(details.bitrateKbps, 0U) << name;
    }
}

TEST_F(FileDetailsTest, TechnicalDataComesFromTheStreamAndTheFile) {
    const auto file = flac();
    const auto details = read(file);
    EXPECT_EQ(details.format, "FLAC");
    EXPECT_EQ(details.sampleRate, 44100U);
    EXPECT_EQ(details.bitsPerSample, 16U);
    EXPECT_EQ(details.channels, 2U);
    EXPECT_EQ(details.frames, 88200U);
    EXPECT_EQ(details.lengthMs, 2000U);
    EXPECT_GT(details.bitrateKbps, 0U);
    EXPECT_EQ(details.size, fs::file_size(file));
    ASSERT_TRUE(details.modified.has_value());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    EXPECT_LT(std::abs(*details.modified - now.count()), 600);
    EXPECT_TRUE(details.folderPresent);
}

TEST_F(FileDetailsTest, ATagTheLibraryShowsOtherwiseIsMarkedWithTheLibrarysValue) {
    const auto file = mp3();
    {
        TagLib::MPEG::File tags(file.c_str());
        tags.ID3v2Tag(true)->setTitle(u8("Shown title"));
        tags.ID3v2Tag()->setArtist(u8("Artist"));
        tags.ID3v2Tag()->setTrack(3);
        tags.ID3v1Tag(true)->setTitle("Hidden title");
        tags.ID3v1Tag()->setArtist("Artist");
        tags.ID3v1Tag()->setTrack(3);
        ASSERT_TRUE(tags.save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v4));
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    auto db = store.openReader();
    const auto details = read(file, &db);
    EXPECT_TRUE(details.inLibrary);
    EXPECT_EQ(details.path, camel::toUtf8(file.wstring()));
    const auto* v1 = sectionOf(details, "ID3v1");
    ASSERT_NE(tagOf(v1, "TITLE"), nullptr);
    EXPECT_EQ(tagOf(v1, "TITLE")->library, "Shown title");
    EXPECT_FALSE(tagOf(v1, "ARTIST")->library.has_value());
    EXPECT_FALSE(tagOf(v1, "TRACKNUMBER")->library.has_value());
    const auto* v2 = sectionOf(details, "ID3v2.4");
    EXPECT_FALSE(tagOf(v2, "TIT2")->library.has_value());
    EXPECT_FALSE(tagOf(v2, "TRCK")->library.has_value());
    // A change since the last scan is marked too.
    {
        TagLib::MPEG::File tags(file.c_str());
        tags.ID3v2Tag()->setTitle(u8("Retitled"));
        ASSERT_TRUE(tags.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, TagLib::ID3v2::v4));
    }
    EXPECT_EQ(tagOf(sectionOf(read(file, &db), "ID3v2.4"), "TIT2")->library, "Shown title");
}

TEST_F(FileDetailsTest, AFileThatIsGoneShowsTheLibrarysFieldsAndWhetherItsFolderIsThere) {
    const auto file = flac();
    {
        TagLib::FLAC::File tags(file.c_str());
        tags.xiphComment(true)->addField("TITLE", u8("Kept title"));
        tags.xiphComment()->addField("ARTIST", u8("Ann"));
        tags.xiphComment()->addField("ARTIST", u8("Bob"), false);
        ASSERT_TRUE(tags.save());
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    auto db = store.openReader();
    fs::remove(file);
    const auto details = read(file, &db);
    EXPECT_EQ(details.problem, FileDetails::Problem::Missing);
    EXPECT_TRUE(details.sections.empty());
    EXPECT_TRUE(details.folderPresent);
    ASSERT_GE(details.library.size(), 2U);
    EXPECT_EQ(details.library[0].field, "TITLE");
    EXPECT_EQ(details.library[0].values, (Values{"Kept title"}));
    EXPECT_EQ(details.library[1].values, (Values{"Ann", "Bob"}));
    EXPECT_EQ(details.format, "FLAC");
    EXPECT_EQ(details.sampleRate, 44100U);
    EXPECT_EQ(details.path, camel::toUtf8(file.wstring()));
    fs::remove_all(dir / "music");
    EXPECT_FALSE(read(file, &db).folderPresent);
}

TEST_F(FileDetailsTest, AFileThatCannotBeDecodedSaysWhy) {
    const auto file = dir / "music" / "broken.flac";
    std::ofstream(file, std::ios::binary) << "not audio at all";
    const auto details = read(file);
    EXPECT_EQ(details.problem, FileDetails::Problem::Unreadable);
    EXPECT_FALSE(details.reason.empty());
    EXPECT_TRUE(details.sections.empty());
    EXPECT_EQ(details.size, 16U);
    EXPECT_TRUE(details.folderPresent);
}

TEST_F(FileDetailsTest, NothingIsWrittenToTheFile) {
    const auto file = mp3();
    {
        TagLib::MPEG::File tags(file.c_str());
        tags.ID3v2Tag(true)->setTitle(u8("Title"));
        ASSERT_TRUE(tags.save());
    }
    const auto size = fs::file_size(file);
    const auto written = fs::last_write_time(file);
    std::ifstream in(file, std::ios::binary);
    const std::string before{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    (void)read(file);
    std::ifstream again(file, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(again), std::istreambuf_iterator<char>()};
    EXPECT_EQ(fs::file_size(file), size);
    EXPECT_EQ(fs::last_write_time(file), written);
    EXPECT_EQ(before, after);
}
