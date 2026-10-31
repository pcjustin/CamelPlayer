// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "FakeHttp.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "Common/util/Text.h"
#include "Server/Artwork/ArtworkCache.h"
#include "Server/Library/ArtistPictures.h"
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
        for (int x = 0; x < width; ++x) put(54 + size_t(y) * stride + x * 3, y < height / 2 ? bottom : top, 3);
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
    for (const auto& request : requests) EXPECT_NE(request.find("User-Agent: CamelPlayer/"), std::string::npos) << request;

    // Found pictures are kept: later visits ask nothing.
    EXPECT_EQ(lookUpArtistPicture(*store, "Amy", true, services, {}), picture);
    EXPECT_EQ(web.requests().size(), 5u);
    ArtworkCache cache(dir / "thumbs");
    const auto thumbnail = cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Amy"});
    ASSERT_TRUE(thumbnail);
    EXPECT_TRUE(std::filesystem::exists(thumbnail->path));
    EXPECT_FALSE(cache.get(store.get(), {ArtworkKind::Artist, 0, {}, "Nobody"}));
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

TEST_F(ArtistPicturesTest, RetriesFailuresAtTheNextVisitAndNothingFoundAfterThirtyDays) {
    web.route("/ws/2/artist?query=", "busy", 503);
    auto picture = lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_FALSE(picture.found);
    EXPECT_EQ(web.requests().size(), 3u); // tried twice more after HTTP 503
    lookUpArtistPicture(*store, "Amy", true, services, {});
    EXPECT_EQ(web.requests().size(), 3u); // not within a minute
    age("Amy", 2 * 60'000);
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
