// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "FakeHttp.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>

#include "Common/util/Text.h"
#include "Server/Artwork/ArtworkCache.h"
#include "Server/Library/ArtistPictures.h"
#include "Server/Library/LibraryScanner.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

// An uncompressed 24-bit BMP whose top half is `top` and bottom half `bottom` (0xRRGGBB).
std::vector<uint8_t> bitmap(int width, int height, uint32_t top, uint32_t bottom) {
    const int stride = (width * 3 + 3) & ~3;
    const uint32_t pixels = uint32_t(stride * height), size = 54 + pixels;
    std::vector<uint8_t> out(size);
    const auto put = [&](size_t at, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out[at + i] = uint8_t(value >> (8 * i));
    };
    out[0] = 'B';
    out[1] = 'M';
    put(2, size, 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, uint32_t(width), 4);
    put(22, uint32_t(height), 4);
    put(26, 1, 2);
    put(28, 24, 2);
    put(34, pixels, 4);
    for (int y = 0; y < height; ++y) // bottom-up rows
        for (int x = 0; x < width; ++x) put(54 + size_t(y) * stride + size_t(x) * 3, y < height / 2 ? bottom : top, 3);
    return out;
}

struct Decoded {
    unsigned width = 0, height = 0;
    uint32_t centre = 0; // 0xRRGGBB
};

Decoded decode(const std::vector<uint8_t>& image) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    EXPECT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    factory->CreateStream(&stream);
    stream->InitializeFromMemory(const_cast<uint8_t*>(image.data()), DWORD(image.size()));
    ComPtr<IWICBitmapDecoder> decoder;
    EXPECT_HRESULT_SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    Decoded out;
    frame->GetSize(&out.width, &out.height);
    ComPtr<IWICFormatConverter> converter;
    factory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom);
    uint8_t bgr[3] = {};
    const WICRect centre{INT(out.width / 2), INT(out.height / 2), 1, 1};
    converter->CopyPixels(&centre, 3, 3, bgr);
    out.centre = uint32_t(bgr[2]) << 16 | uint32_t(bgr[1]) << 8 | bgr[0];
    return out;
}

bool similar(uint32_t color, uint32_t expected) {
    for (int shift : {0, 8, 16})
        if (std::abs(int((color >> shift) & 255) - int((expected >> shift) & 255)) > 24) return false;
    return true;
}

std::string bytesText(const std::vector<uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}

constexpr char kMbid[] = "11111111-1111-1111-1111-111111111111";
constexpr char kOther[] = "22222222-2222-2222-2222-222222222222";

class ArtistPicturesTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;
    testutil::FakeHttp web;
    online::Services services;

    void SetUp() override {
        dir = testutil::tempDir() / "artist-pictures" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir);
        store = std::make_unique<LibraryStore>(dir / "data");
        services.musicbrainz = services.wikidata = services.commons = web.base();
        services.musicbrainzInterval = std::chrono::milliseconds(0);
        services.backoff = std::chrono::milliseconds(10);
        services.allowHttp = true;
    }

    // MusicBrainz knows `mbid` as `name`; Wikidata gives it a picture that Commons serves.
    void publish(const std::string& mbid, const std::string& item, uint32_t color) {
        web.route("/ws/2/artist/" + mbid + "?inc=url-rels&fmt=json",
                  R"({"relations":[{"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/)" + item + R"("}}]})");
        web.route("/w/api.php?action=wbgetclaims&entity=" + item,
                  R"({"claims":{"P18":[{"mainsnak":{"datavalue":{"value":")" + item + R"(.jpg"}},"rank":"normal"}]}})");
        web.route(
            "/w/api.php?action=query&titles=File%3A" + item,
            R"({"query":{"pages":[{"imageinfo":[{"thumburl":")" + web.base() + "/image/" + item +
                R"(","descriptionurl":"https://commons.wikimedia.org/wiki/File:)" + item +
                R"(.jpg","extmetadata":{"Artist":{"value":"<a href=\"x\">Jane &amp; Co</a>"},"LicenseShortName":{"value":"CC BY-SA 4.0"},"LicenseUrl":{"value":"https://creativecommons.org/licenses/by-sa/4.0"}}}]}]}})");
        web.route("/image/" + item, bytesText(bitmap(40, 80, color, 0x0000FF)));
    }

    void search(const std::string& artists) { web.route("/ws/2/artist?query=", R"({"artists":[)" + artists + "]}"); }
    // MusicBrainz's answer to a search for `name`, a single word.
    void searchFor(const std::string& name, const std::string& artists) {
        web.route("/ws/2/artist?query=artist%3A%22" + name + "%22", R"({"artists":[)" + artists + "]}");
    }
    static std::string artist(const std::string& mbid, const std::string& name, const std::string& about = {}) {
        return R"({"id":")" + mbid + R"(","name":")" + name + R"(","disambiguation":")" + about + R"("})";
    }
    // The names MusicBrainz was asked about, in order.
    std::vector<std::string> searched() {
        std::vector<std::string> names;
        const std::regex query(R"(/ws/2/artist\?query=artist%3A%22([^%]+)%22)");
        for (const auto& request : web.requests())
            if (std::smatch m; std::regex_search(request, m, query)) names.push_back(m[1]);
        return names;
    }

    // A track credited to `artist`, and to `composer` when given, on an album of its own.
    std::filesystem::path addTrack(const std::string& artist, const std::string& composer = {}) {
        const auto music = dir / "music";
        std::filesystem::create_directories(music);
        const auto file = music / (artist + ".flac");
        std::filesystem::copy_file(testutil::testFile("16-44.1k"), file);
        TagLib::FLAC::File tags(file.c_str());
        tags.tag()->setArtist(TagLib::String(artist, TagLib::String::UTF8));
        tags.tag()->setAlbum(TagLib::String(artist + " Album", TagLib::String::UTF8));
        if (!composer.empty()) {
            auto properties = tags.properties();
            properties.insert("COMPOSER", TagLib::StringList(TagLib::String(composer, TagLib::String::UTF8)));
            tags.setProperties(properties);
        }
        EXPECT_TRUE(tags.save());
        return file;
    }
    void scanMusic() {
        LibraryScanner scanner(*store);
        ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    }
    ArtistPictureJob job(bool online = true) {
        ArtistPictureJob job;
        job.online = [online] { return online; };
        job.services = services;
        return job;
    }

    void age(const std::string& name, int64_t milliseconds) {
        store
            ->write([&](sqlite::Database& db) {
                db.prepare("UPDATE artist_pictures SET checked=checked-? WHERE name=?").bind(1, milliseconds).bind(2, name).run();
            })
            .get();
    }
};

} // namespace

TEST(ArtistPictureParsing, MatchesExactNamesAndAliasesOnly) {
    const auto json = R"({"artists":[
        {"id":"a","name":"梶浦由記","type":"Person","aliases":[{"name":"Yuki Kajiura"}],"area":{"name":"Japan"},
         "life-span":{"begin":"1965-08-06"}},
        {"id":"b","name":"yuki kajiura"},
        {"id":"c","name":"Yuki Kajiura","disambiguation":"someone else"}]})";
    const auto found = online::matchingArtists(json, "Yuki Kajiura");
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], (ArtistCandidate{"a", "梶浦由記", "Person · Japan · 1965"}));
    EXPECT_EQ(found[1], (ArtistCandidate{"c", "Yuki Kajiura", "someone else"}));
    EXPECT_TRUE(online::matchingArtists(R"({"artists":[]})", "Nobody").empty());
}

// 23 MusicBrainz artists are named John Williams (OP-R17): the one MusicBrainz credits with the library's album comes
// first, then those whose years and genres fit the library's tracks; each says what tells it apart.
TEST(ArtistPictureParsing, RanksTheArtistsOfANameByWhatTheLibraryKnows) {
    const auto json = R"({"artists":[
        {"id":"guitarist","name":"John Williams","score":100,"type":"Person","disambiguation":"classical guitarist",
         "area":{"name":"Australia"},"life-span":{"begin":"1941-04-24"},"tags":[{"name":"classical"}]},
        {"id":"rapper","name":"John Williams","score":95,"type":"Person","life-span":{"begin":"2004"}},
        {"id":"composer","name":"John Williams","score":90,"type":"Person","disambiguation":"film composer",
         "area":{"name":"United States"},"life-span":{"begin":"1932-02-08"},"tags":[{"name":"soundtrack"},{"name":"film score"}]},
        {"id":"singer","name":"John Williams","score":80,"type":"Person","life-span":{"begin":"1930","end":"1990","ended":true}}]})";
    online::ArtistClues clues{{"Harry Potter and the Chamber of Secrets"}, 2002, 2002, {"Soundtrack"}};
    auto ranked = online::rankedArtists(json, "John Williams", clues, {{"composer", 1}});
    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0], (ArtistCandidate{"composer", "John Williams", "film composer · Person · United States · 1932", 1}));
    EXPECT_EQ(ranked[3].mbid, "rapper"); // born after the album
    EXPECT_EQ(ranked[2].description, "Person · 1930–1990");
    // Without the album's credit, the genre still tells.
    ranked = online::rankedArtists(json, "John Williams", clues, {});
    EXPECT_EQ(ranked[0].mbid, "composer");
    EXPECT_EQ(ranked[0].albums, 0);
    // With nothing known, MusicBrainz's own order stands.
    ranked = online::rankedArtists(json, "John Williams", {}, {});
    EXPECT_EQ(ranked[0].mbid, "guitarist");
    EXPECT_EQ(ranked[1].mbid, "rapper");
}

TEST(ArtistPictureParsing, ReadsTheArtistsCreditedOnReleaseGroups) {
    const auto credited = online::creditedArtists(R"({"release-groups":[
        {"title":"Harry Potter","artist-credit":[{"name":"John Williams","artist":{"id":"composer"}},
                                                 {"name":"London Symphony Orchestra","artist":{"id":"lso"}}]},
        {"title":"Harry Potter","artist-credit":[{"artist":{"id":"composer"}}]},
        {"title":"Other"}]})");
    EXPECT_EQ(credited, (std::vector<std::string>{"composer", "lso"}));
}

TEST(ArtistPictureParsing, ReadsWikidataPicturesAndCommonsAttribution) {
    EXPECT_EQ(online::wikidataItem(R"({"relations":[{"type":"discogs","url":{"resource":"https://www.discogs.com/artist/1"}},
        {"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/Q42"}}]})"),
              "Q42");
    EXPECT_FALSE(online::wikidataItem(R"({"relations":[]})"));
    EXPECT_EQ(online::pictureFile(R"({"claims":{"P18":[
        {"mainsnak":{"datavalue":{"value":"Old.jpg"}},"rank":"deprecated"},
        {"mainsnak":{"datavalue":{"value":"Normal.jpg"}},"rank":"normal"},
        {"mainsnak":{"datavalue":{"value":"Best.jpg"}},"rank":"preferred"}]}})"),
              "Best.jpg");
    EXPECT_FALSE(online::pictureFile(R"({"claims":{}})"));
    const auto image = online::commonsImage(R"({"query":{"pages":[{"imageinfo":[{"thumburl":"https://upload/x.jpg",
        "descriptionurl":"https://commons/File:X.jpg","extmetadata":{"Artist":{"value":"<span>Ann&#233; &lt;3</span>\n"},
        "LicenseShortName":{"value":"CC0"},"LicenseUrl":{"value":"https://cc0"}}}]}]}})");
    ASSERT_TRUE(image);
    EXPECT_EQ(image->url, "https://upload/x.jpg");
    EXPECT_EQ(image->page, "https://commons/File:X.jpg");
    EXPECT_EQ(image->author, "Anné <3");
    EXPECT_EQ(image->license, "CC0");
    EXPECT_FALSE(online::commonsImage(R"({"query":{"pages":[{"missing":true}]}})"));
}

TEST(ArtistPictureImage, CropsTheMiddleOfWidePicturesAndTheTopOfTallOnes) {
    const auto tall = decode(squarePicture(bitmap(60, 120, 0xFF0000, 0x0000FF), 512));
    EXPECT_EQ(tall.width, 60u);
    EXPECT_EQ(tall.height, 60u);
    EXPECT_TRUE(similar(tall.centre, 0xFF0000)) << std::hex << tall.centre;
    const auto wide = decode(squarePicture(bitmap(1200, 600, 0x00FF00, 0x00FF00), 512));
    EXPECT_EQ(wide.width, 512u);
    EXPECT_EQ(wide.height, 512u);
    EXPECT_THROW(squarePicture({1, 2, 3}, 512), std::invalid_argument);
}

TEST_F(ArtistPicturesTest, AsksNothingOnlineWhileOffAndKeepsWhatItFinds) {
    search(R"({"id":")" + std::string(kMbid) + R"(","name":"Amy"})");
    publish(kMbid, "Q1", 0xFF0000);
    auto picture = lookUpArtistPicture(*store, "Amy", false, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_TRUE(web.requests().empty());

    picture = lookUpArtistPicture(*store, "Amy", true, services, {});
    ASSERT_TRUE(picture.found);
    EXPECT_EQ(picture.source, "commons");
    EXPECT_EQ(picture.mbid, kMbid);
    EXPECT_EQ(picture.author, "Jane & Co");
    EXPECT_EQ(picture.license, "CC BY-SA 4.0");
    EXPECT_EQ(picture.page, "https://commons.wikimedia.org/wiki/File:Q1.jpg");
    const auto requests = web.requests();
    ASSERT_EQ(requests.size(), 5u);
    // The Server's own version, as MusicBrainz and Wikimedia ask; a Server built without CAMEL_VERSION sent its name.
    const std::regex agent(R"(User-Agent: CamelPlayer/\d+\.\d+\.\d+ \()");
    for (const auto& request : requests) EXPECT_TRUE(std::regex_search(request, agent)) << request;

    // Found pictures are kept: later visits ask nothing.
    EXPECT_EQ(lookUpArtistPicture(*store, "Amy", true, services, {}), picture);
    EXPECT_EQ(web.requests().size(), 5u);
    ArtworkCache cache(dir / "thumbs");
    const auto thumbnail = cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Amy"});
    ASSERT_TRUE(thumbnail);
    EXPECT_TRUE(std::filesystem::exists(thumbnail->path));
    EXPECT_FALSE(cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Nobody"}));
}

// A lookup cancelled while it waits for its MusicBrainz turn ends at once, not after the turns ahead of it.
TEST_F(ArtistPicturesTest, ACancelledLookupDoesNotWaitForTheTurnsAheadOfIt) {
    publish(kMbid, "Q1", 0xFF0000);
    services.musicbrainzInterval = std::chrono::milliseconds(1500);
    online::pictureOf(kMbid, services, {});
    std::thread waiting([&] { online::pictureOf(kMbid, services, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CancellationToken token;
    token.cancel();
    const auto before = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(online::pictureOf(kMbid, services, token));
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(500));
    waiting.join();
}

TEST_F(ArtistPicturesTest, LeavesSeveralArtistsOfANameForTheUserToChoose) {
    search(R"({"id":")" + std::string(kMbid) +
           R"(","name":"John Williams","disambiguation":"film composer"},)"
           R"({"id":")" +
           std::string(kOther) + R"(","name":"John Williams","disambiguation":"guitarist"})");
    publish(kOther, "Q2", 0x00FF00);
    web.route("/ws/2/artist/" + std::string(kMbid) + "?inc=url-rels&fmt=json", R"({"relations":[]})");
    auto picture = lookUpArtistPicture(*store, "John Williams", true, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_TRUE(picture.mbid.empty());
    ASSERT_EQ(picture.candidates.size(), 2u);
    EXPECT_EQ(picture.candidates[1].description, "guitarist");
    EXPECT_EQ(web.requests().size(), 1u);
    // Not searched again before 30 days.
    lookUpArtistPicture(*store, "John Williams", true, services, {});
    EXPECT_EQ(web.requests().size(), 1u);

    EXPECT_THROW(chooseArtistCandidate(*store, "John Williams", "elsewhere", services, {}), std::invalid_argument);
    picture = chooseArtistCandidate(*store, "John Williams", kOther, services, {});
    EXPECT_TRUE(picture.found);
    EXPECT_EQ(picture.mbid, kOther);
    EXPECT_EQ(picture.candidates.size(), 2u); // another can still be chosen
    // The other one has no picture, so choosing it removes this one.
    picture = chooseArtistCandidate(*store, "John Williams", kMbid, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_EQ(picture.mbid, kMbid);
}

// The library's album by the name is looked up on MusicBrainz to tell which artist it is; the order and the albums are
// kept with the candidates.
TEST_F(ArtistPicturesTest, OrdersSeveralArtistsOfANameByTheLibrarysAlbums) {
    search(R"({"id":")" + std::string(kOther) + R"(","name":"John Williams","disambiguation":"guitarist"},)" + R"({"id":")" +
           std::string(kMbid) + R"(","name":"John Williams","disambiguation":"film composer"})");
    web.route("/ws/2/release-group?query=",
              R"({"release-groups":[{"artist-credit":[{"artist":{"id":")" + std::string(kMbid) + R"("}}]}]})");
    const auto music = dir / "music";
    std::filesystem::create_directories(music);
    const auto file = music / "one.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), file);
    {
        TagLib::FLAC::File tags(file.c_str());
        tags.tag()->setArtist("John Williams");
        tags.tag()->setAlbum("Harry Potter and the Chamber of Secrets");
        tags.tag()->setGenre("Soundtrack");
        tags.tag()->setYear(2002);
        ASSERT_TRUE(tags.save());
    }
    LibraryScanner scanner(*store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(music)).complete);
    const auto picture = lookUpArtistPicture(*store, "John Williams", true, services, {});
    ASSERT_EQ(picture.candidates.size(), 2u);
    EXPECT_EQ(picture.candidates[0].mbid, kMbid);
    EXPECT_EQ(picture.candidates[0].albums, 1);
    const auto requests = web.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_NE(requests[1].find("Chamber%20of%20Secrets"), std::string::npos) << requests[1];
    EXPECT_EQ(artistPicture(*store, "John Williams").candidates, picture.candidates);
}

// A name's artists kept unordered by an earlier version are searched again at the next visit, not after thirty days.
TEST_F(ArtistPicturesTest, ArtistsKeptBeforeTheyWereOrderedAreSearchedAgain) {
    search(R"({"id":")" + std::string(kOther) + R"(","name":"John Williams","disambiguation":"guitarist"},)" + R"({"id":")" +
           std::string(kMbid) + R"(","name":"John Williams","disambiguation":"film composer"})");
    store
        ->write([](sqlite::Database& db) {
            db.prepare("INSERT INTO artist_pictures(name,outcome,online,checked,candidates) VALUES(?,'ambiguous',1,?,?)")
                .bind(1, std::string("John Williams"))
                .bind(2,
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                          .count())
                .bind(3, std::string(R"([{"mbid":"a","name":"John Williams","description":"x"},)"
                                     R"({"mbid":"b","name":"John Williams","description":"y"}])"))
                .run();
        })
        .get();
    const auto picture = lookUpArtistPicture(*store, "John Williams", true, services, {});
    EXPECT_EQ(web.requests().size(), 1u);
    ASSERT_EQ(picture.candidates.size(), 2u);
    EXPECT_EQ(picture.candidates[0].mbid, kOther);
    // Once ordered, they are kept as before.
    lookUpArtistPicture(*store, "John Williams", true, services, {});
    EXPECT_EQ(web.requests().size(), 1u);
}

// On the real MusicBrainz, with what the owner's library knows of John Williams (OP-R17, 2026-11-18).
TEST(ArtistPicturesOnline, DISABLED_OrdersJohnWilliamsByTheLibrarysAlbum) {
    const online::ArtistClues clues{{"Harry Potter and the Chamber of Secrets"}, 2002, 2002, {"Soundtrack"}};
    const auto found = online::findArtistPicture("John Williams", clues, {}, {});
    ASSERT_GT(found.candidates.size(), 1u);
    for (const auto& candidate : found.candidates)
        std::cout << candidate.albums << "  " << candidate.mbid << "  " << candidate.description << "\n";
    EXPECT_EQ(found.candidates.front().albums, 1);
}

TEST_F(ArtistPicturesTest, RetriesFailuresAtTheNextVisitAndNothingFoundAfterThirtyDays) {
    web.route("/ws/2/artist?query=", "busy", 503);
    auto picture = lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_EQ(web.requests().size(), 3u); // tried twice more after HTTP 503
    lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_EQ(web.requests().size(), 3u); // not within a minute
    age("Amy", int64_t{2} * 60'000);
    search("");
    picture = lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_EQ(web.requests().size(), 4u);
    lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_EQ(web.requests().size(), 4u);
    age("Amy", 31ll * 24 * 3600'000);
    lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_EQ(web.requests().size(), 5u);

    // A cancelled lookup keeps nothing.
    CancellationToken token;
    token.cancel();
    lookUpArtistPicture(*store, "Zed", true, services, token);
    auto db = store->openReader();
    EXPECT_FALSE(db.prepare("SELECT 1 FROM artist_pictures WHERE name='Zed'").step());
}

// OP-R59: the library's names are looked up in the background, artists and composers alike, without their pages
// opened: one MusicBrainz artist of the name gives its picture, several are kept for the user to choose and none is kept
// as none; the user's own picture stays and its name is asked about nowhere.
TEST_F(ArtistPicturesTest, TheBackgroundFindsPicturesWithoutTheirPagesOpened) {
    addTrack("Amy");
    addTrack("Juno");
    addTrack("Nix", "Cleo");
    addTrack("Zed");
    scanMusic();
    searchFor("Amy", artist(kMbid, "Amy"));
    publish(kMbid, "Q1", 0xFF0000);
    searchFor("Juno", artist(kMbid, "Juno", "singer") + "," + artist(kOther, "Juno", "band"));
    searchFor("Nix", "");
    searchFor("Cleo", artist(kOther, "Cleo"));
    publish(kOther, "Q2", 0x00FF00);
    ASSERT_TRUE(setArtistPicture(*store, "Zed", bitmap(20, 20, 0x0000FF, 0x0000FF)).chosen);

    const auto counts = findArtistPictures(*store, job(), {});
    EXPECT_EQ(counts.found, 2u);
    EXPECT_EQ(counts.ambiguous, 1u);
    EXPECT_EQ(counts.none, 1u);
    EXPECT_EQ(artistPicture(*store, "Amy").source, "commons");
    EXPECT_EQ(artistPicture(*store, "Cleo").mbid, kOther);
    const auto juno = artistPicture(*store, "Juno");
    EXPECT_FALSE(juno.found);
    EXPECT_TRUE(juno.mbid.empty());
    EXPECT_EQ(juno.candidates.size(), 2u);
    EXPECT_FALSE(artistPicture(*store, "Nix").found);
    const auto zed = artistPicture(*store, "Zed");
    EXPECT_TRUE(zed.chosen);
    EXPECT_FALSE(zed.found);
    EXPECT_EQ(std::ranges::count(searched(), "Zed"), 0);

    // What was kept is not looked up again.
    const auto asked = web.requests().size();
    const auto again = findArtistPictures(*store, job(), {});
    EXPECT_EQ(again.found + again.ambiguous + again.none, 0u);
    EXPECT_EQ(web.requests().size(), asked);
}

// Favorite names go first, then the names most played, then the rest by name.
TEST_F(ArtistPicturesTest, TheBackgroundTakesFavoritesThenTheMostPlayedThenTheRestByName) {
    for (const auto* name : {"Bea", "Cid", "Dot", "Amy"}) addTrack(name);
    scanMusic();
    search("");
    store
        ->write([](sqlite::Database& db) {
            db.exec("INSERT INTO favorite_names(field,name,added) VALUES('artist','Dot',0);"
                    "INSERT INTO stats(file_id,play_count) SELECT file_id,5 FROM tracks WHERE artist='Cid';");
        })
        .get();
    findArtistPictures(*store, job(), {});
    EXPECT_EQ(searched(), (std::vector<std::string>{"Dot", "Cid", "Amy", "Bea"}));
}

// While the settings do not allow it, the background looks only in the library and asks no one; once they do, the
// names it found nothing for are looked up online.
TEST_F(ArtistPicturesTest, TheBackgroundGoesOnlineOnlyOnceAllowed) {
    addTrack("Amy");
    scanMusic();
    searchFor("Amy", artist(kMbid, "Amy"));
    publish(kMbid, "Q1", 0xFF0000);
    EXPECT_EQ(findArtistPictures(*store, job(false), {}).none, 1u);
    EXPECT_TRUE(web.requests().empty());
    EXPECT_EQ(findArtistPictures(*store, job(true), {}).found, 1u);
    EXPECT_TRUE(artistPicture(*store, "Amy").found);
}

// A page's own lookup, which LocalServer counts while it goes on, comes before the background's next name.
TEST_F(ArtistPicturesTest, TheBackgroundStartsNoNameWhileAPageLooksUp) {
    addTrack("Amy");
    scanMusic();
    search("");
    std::atomic<bool> page = true;
    auto waiting = job();
    waiting.waiting = [&] { return page.load(); };
    std::thread run([&] { findArtistPictures(*store, waiting, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_TRUE(web.requests().empty());
    // A name a scan adds meanwhile is among those looked up: the names are read once the wait ends.
    addTrack("Bea");
    scanMusic();
    page = false;
    run.join();
    EXPECT_EQ(searched(), (std::vector<std::string>{"Amy", "Bea"}));

    // Stopped while it waits, it ends at once.
    addTrack("Cid");
    scanMusic();
    page = true;
    CancellationToken token;
    std::thread stopped([&] { findArtistPictures(*store, waiting, token); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    token.cancel();
    stopped.join();
    EXPECT_EQ(searched(), (std::vector<std::string>{"Amy", "Bea"}));
}

// The yield rule (docs/Plan.md §7.7): a name waits while a zone plays from its storage without enough ahead, and goes
// on once nothing plays there.
TEST_F(ArtistPicturesTest, TheBackgroundWaitsWhilePlaybackNeedsTheNamesStorage) {
    const auto track = addTrack("Amy");
    scanMusic();
    search("");
    std::mutex mutex;
    PlaybackReads reads;
    reads.files = {track};
    std::vector<std::filesystem::path> paced;
    auto yielding = job();
    yielding.pace = [&](const std::filesystem::path& storage) {
        std::lock_guard lock(mutex);
        paced.push_back(storage);
        return backgroundPace(reads, storage);
    };
    std::thread run([&] { findArtistPictures(*store, yielding, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_TRUE(web.requests().empty());
    {
        std::lock_guard lock(mutex);
        reads.files.clear();
    }
    run.join();
    EXPECT_EQ(searched(), std::vector<std::string>{"Amy"});
    ASSERT_FALSE(paced.empty());
    EXPECT_TRUE(std::filesystem::equivalent(paced.front(), track)) << paced.front();
}

TEST_F(ArtistPicturesTest, PrefersTheUsersOwnPictureAndAFolderPictureOverTheInternet) {
    const auto folder = dir / "music" / "Amy";
    std::filesystem::create_directories(folder);
    const auto image = bitmap(30, 30, 0x00FF00, 0x00FF00);
    std::ofstream(folder / "Artist.jpg", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    store
        ->write([&](sqlite::Database& db) {
            db.exec("INSERT INTO roots(id,path,added,online) VALUES(1,'c:\\m',0,1);");
            db.prepare("INSERT INTO folders(id,root_id,path,name) VALUES(1,1,?,'Amy')")
                .bind(1, camel::toUtf8(folder.wstring()))
                .run();
            db.prepare("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES(1,1,1,?,'image',1,1,1)")
                .bind(1, camel::toUtf8((folder / "artist.jpg").wstring()))
                .run();
        })
        .get();
    auto picture = lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_TRUE(picture.found);
    EXPECT_EQ(picture.source, "folder");
    EXPECT_TRUE(web.requests().empty());

    ArtworkCache cache(dir / "thumbs");
    const auto before = cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Amy"});
    picture = setArtistPicture(*store, "Amy", bitmap(20, 20, 0xFF0000, 0xFF0000));
    EXPECT_TRUE(picture.chosen);
    const auto chosen = cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Amy"});
    ASSERT_TRUE(before && chosen);
    EXPECT_NE(chosen->key, before->key); // a new picture is a new thumbnail
    EXPECT_THROW(setArtistPicture(*store, "Amy", {1, 2, 3}), std::invalid_argument);

    picture = removeArtistPicture(*store, "Amy");
    EXPECT_FALSE(picture.chosen);
    EXPECT_TRUE(picture.found); // back to the folder's picture
    // The user's picture alone, with nothing looked up yet.
    EXPECT_TRUE(setArtistPicture(*store, "Zed", bitmap(20, 20, 0xFF0000, 0xFF0000)).chosen);
    EXPECT_TRUE(cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Zed"}));
}
