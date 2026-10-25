// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>
#include <webp/encode.h>
#include <fstream>
#include "Server/Artwork/ArtworkCache.h"
#include "Server/Library/LibraryScanner.h"
#include "Common/util/Text.h"
#include "TestUtil.h"

using namespace camel::server;
namespace {
using Path = std::filesystem::path;
ArtworkCandidate bitmap(unsigned width = 800, unsigned height = 400, uint8_t color = 70) {
    ArtworkCandidate image;
    auto& b = image.bytes;
    const unsigned stride = (width * 3 + 3) & ~3u;
    b.resize(54 + size_t(stride) * height, color);
    std::fill_n(b.begin(), 54, uint8_t(0));
    auto put = [&](size_t offset, unsigned value) {
        for (size_t i = 0; i < 4; ++i) b[offset + i] = uint8_t(value >> (i * 8));
    };
    b[0] = 'B';
    b[1] = 'M';
    put(2, unsigned(b.size()));
    put(10, 54);
    put(14, 40);
    put(18, width);
    put(22, height);
    b[26] = 1;
    b[28] = 24;
    return image;
}
void save(const Path& file, const ArtworkCandidate& image) {
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(image.bytes.data()), std::streamsize(image.bytes.size()));
}
std::pair<unsigned, unsigned> dimensions(const Path& file) {
    using Microsoft::WRL::ComPtr;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    frame.Reset();
    decoder.Reset();
    factory.Reset();
    if (SUCCEEDED(hr)) CoUninitialize();
    return {w, h};
}
class ArtworkCacheTest : public testing::Test {
protected:
    Path dir = testutil::tempDir() / "artwork-cache";
    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "music" / "CD1");
    }
};
} // namespace
TEST_F(ArtworkCacheTest, GeneratesBothSizesPreservingAspectAndReusesTheCache) {
    ArtworkCache cache(dir / "thumbs");
    const auto image = bitmap();
    const auto smallRef = cache.thumbnail(image, 256);
    const auto large = cache.thumbnail(image, 600);
    ASSERT_TRUE(smallRef);
    ASSERT_TRUE(large);
    EXPECT_EQ(dimensions(smallRef->path), (std::pair<unsigned, unsigned>{256, 128}));
    EXPECT_EQ(dimensions(large->path), (std::pair<unsigned, unsigned>{600, 300}));
    EXPECT_NE(smallRef->key, large->key);
    const auto reused = cache.thumbnail(image, 256);
    ASSERT_TRUE(reused);
    EXPECT_EQ(smallRef->key, reused->key);
    EXPECT_EQ(smallRef->path, reused->path);
    EXPECT_EQ(smallRef->path.extension(), L".jpg");
}
TEST_F(ArtworkCacheTest, RejectsCorruptOversizedDimensionsAndCancellationWithoutPublishing) {
    ArtworkCache cache(dir / "thumbs");
    ArtworkCandidate corrupt;
    corrupt.bytes = {1, 2, 3};
    EXPECT_FALSE(cache.thumbnail(corrupt, 256));
    auto huge = bitmap(4, 4);
    huge.bytes[18] = 0xff;
    huge.bytes[19] = 0xff;
    EXPECT_FALSE(cache.thumbnail(huge, 256));
    ArtworkCandidate oversized;
    oversized.bytes.resize(size_t(kMaxArtworkBytes) + 1);
    EXPECT_FALSE(cache.thumbnail(oversized, 256));
    auto image = bitmap();
    EXPECT_FALSE(cache.thumbnail(image, 128));
    CancellationToken token;
    token.cancel();
    EXPECT_FALSE(cache.thumbnail(image, 256, token));
    for (const auto& entry : std::filesystem::directory_iterator(dir / "thumbs")) ADD_FAILURE() << entry.path();
}
TEST_F(ArtworkCacheTest, FileVersionChangesAndEmbeddedDigestsInvalidateTheKey) {
    ArtworkCache cache(dir / "thumbs");
    const auto file = dir / "cover.png";
    save(file, bitmap());
    ArtworkCandidate external;
    external.file = file;
    const auto first = cache.thumbnail(external, 256);
    ASSERT_TRUE(first);
    save(file, bitmap(100, 100, 200));
    const auto changed = cache.thumbnail(external, 256);
    ASSERT_TRUE(changed);
    EXPECT_NE(first->key, changed->key);
    const auto embedded1 = cache.thumbnail(bitmap(100, 100, 70), 256);
    const auto embedded2 = cache.thumbnail(bitmap(100, 100, 80), 256);
    ASSERT_TRUE(embedded1);
    ASSERT_TRUE(embedded2);
    EXPECT_NE(embedded1->key, embedded2->key);
}
TEST_F(ArtworkCacheTest, DiskCacheEvictsOldThumbnailsToItsByteLimit) {
    ArtworkCache probe(dir / "probe");
    const auto first = probe.thumbnail(bitmap(), 256);
    ASSERT_TRUE(first);
    const auto limit = std::filesystem::file_size(first->path) + 10;
    ArtworkCache cache(dir / "thumbs", limit);
    ASSERT_TRUE(cache.thumbnail(bitmap(), 256));
    ASSERT_TRUE(cache.thumbnail(bitmap(800, 400, 100), 256));
    uintmax_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "thumbs")) total += entry.file_size();
    EXPECT_LE(total, limit);
}
TEST_F(ArtworkCacheTest, WebpWorksWithoutTheWindowsCodec) {
    ArtworkCache cache(dir / "thumbs");
    std::vector<uint8_t> rgba(32 * 16 * 4, 120);
    uint8_t* output = nullptr;
    const auto size = WebPEncodeLosslessRGBA(rgba.data(), 32, 16, 32 * 4, &output);
    ASSERT_GT(size, 0u);
    ArtworkCandidate image;
    image.bytes.assign(output, output + size);
    WebPFree(output);
    const auto ref = cache.thumbnail(image, 256);
    ASSERT_TRUE(ref);
    EXPECT_EQ(dimensions(ref->path), (std::pair<unsigned, unsigned>{32, 16}));
}
TEST_F(ArtworkCacheTest, PlaybackPrefersEmbeddedThenFolderThenDiscParent) {
    ArtworkCache cache(dir / "thumbs");
    const auto file = dir / "music" / "CD1" / "song.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), file);
    const auto front = bitmap(80, 40, 70), folder = bitmap(40, 80, 100);
    {
        TagLib::FLAC::File tags(file.c_str());
        auto* back = new TagLib::FLAC::Picture;
        back->setType(TagLib::FLAC::Picture::BackCover);
        back->setData(TagLib::ByteVector(reinterpret_cast<const char*>(folder.bytes.data()), unsigned(folder.bytes.size())));
        tags.addPicture(back);
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/bmp");
        picture->setData(TagLib::ByteVector(reinterpret_cast<const char*>(front.bytes.data()), unsigned(front.bytes.size())));
        tags.addPicture(picture);
        ASSERT_TRUE(tags.save());
    }
    save(file.parent_path() / "cover.jpg", folder);
    const ArtworkRequest request{ArtworkKind::Track, 0, camel::toUtf8(file.wstring())};
    const auto renamed = dir / "music" / "wrong-extension.wav";
    std::filesystem::copy_file(file, renamed);
    const auto renamedCover = cache.get(nullptr, {ArtworkKind::Track, 0, camel::toUtf8(renamed.wstring())});
    ASSERT_TRUE(renamedCover);
    EXPECT_EQ(dimensions(renamedCover->path), (std::pair<unsigned, unsigned>{80, 40}));
    const auto preferred = cache.get(nullptr, request);
    ASSERT_TRUE(preferred);
    EXPECT_EQ(dimensions(preferred->path), (std::pair<unsigned, unsigned>{80, 40}));
    {
        TagLib::FLAC::File tags(file.c_str());
        tags.removePictures();
        ASSERT_TRUE(tags.save());
    }
    const auto fallback = cache.get(nullptr, request);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(dimensions(fallback->path), (std::pair<unsigned, unsigned>{40, 80}));
    std::filesystem::rename(file.parent_path() / "cover.jpg", dir / "music" / "cover.jpg");
    const auto parent = cache.get(nullptr, request);
    ASSERT_TRUE(parent);
    EXPECT_EQ(dimensions(parent->path), (std::pair<unsigned, unsigned>{40, 80}));
}
TEST_F(ArtworkCacheTest, AlbumAndFolderUseOwnImagesThenTheFirstAlbumAndHideOfflineRoots) {
    const auto music = dir / "music";
    const auto disc = music / "CD1";
    save(disc / "cover.jpg", bitmap(80, 40));
    const auto song = disc / "song.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), song);
    LibraryStore store(dir / "db");
    store
        .write([&](camel::server::sqlite::Database& db) {
            db.exec("INSERT INTO roots(id,path,online,added) VALUES(1,'test',1,0)");
            db.prepare("INSERT INTO folders(id,root_id,path) VALUES(1,1,?)").bind(1, camel::toUtf8(music.wstring())).run();
            db.prepare("INSERT INTO folders(id,root_id,parent_id,path) VALUES(2,1,1,?)")
                .bind(1, camel::toUtf8(disc.wstring()))
                .run();
            db.exec("INSERT INTO albums(id,folder_id,grouping_key,title,added) VALUES(1,1,'album','Album',0)");
            db.prepare("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) VALUES(1,2,1,?,'audio',1,1)")
                .bind(1, camel::toUtf8(song.wstring()))
                .run();
            db.exec("INSERT INTO tracks(id,file_id,album_id,title) VALUES(1,1,1,'Song')");
        })
        .get();
    ArtworkCache cache(dir / "thumbs");
    const auto album = cache.get(&store, {ArtworkKind::Album, 1});
    const auto root = cache.get(&store, {ArtworkKind::Folder, 1});
    ASSERT_TRUE(album);
    ASSERT_TRUE(root);
    EXPECT_EQ(album->key, root->key);
    save(music / "cover.png", bitmap(40, 80));
    const auto own = cache.get(&store, {ArtworkKind::Folder, 1});
    ASSERT_TRUE(own);
    EXPECT_EQ(dimensions(own->path), (std::pair<unsigned, unsigned>{40, 80}));
    store.write([](camel::server::sqlite::Database& db) { db.exec("UPDATE roots SET online=0"); }).get();
    EXPECT_FALSE(cache.get(&store, {ArtworkKind::Album, 1}));
    EXPECT_FALSE(cache.get(&store, {ArtworkKind::Folder, 1}));
}
TEST_F(ArtworkCacheTest, ScannedCoversAreServedWithoutReadingTheirSourceAgain) {
    const auto folder = dir / "music" / "Folder album", embedded = dir / "music" / "Embedded album";
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(embedded);
    save(folder / "cover.jpg", bitmap(80, 40));
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), folder / "song.flac");
    const auto song = embedded / "song.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), song);
    {
        TagLib::FLAC::File tags(song.c_str());
        TagLib::PropertyMap values;
        values.insert("ALBUM", TagLib::StringList("Embedded"));
        tags.setProperties(values);
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/bmp");
        const auto bytes = bitmap(40, 80).bytes;
        picture->setData(TagLib::ByteVector(reinterpret_cast<const char*>(bytes.data()), unsigned(bytes.size())));
        tags.addPicture(picture);
        ASSERT_TRUE(tags.save());
    }
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    ASSERT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM albums WHERE cover_id IS NOT NULL").value(), 2);

    std::vector<int64_t> albums;
    for (auto rows = store.openReader().prepare("SELECT id FROM albums ORDER BY id"); rows.step();) albums.push_back(rows.integer(0));
    ArtworkCache cache(dir / "thumbs");
    EXPECT_EQ(cache.prepare(store, {}), 2u);
    EXPECT_EQ(cache.prepare(store, {}), 0u);
    std::vector<ArtworkReference> covers;
    for (const auto album : albums) {
        auto cover = cache.get(&store, {ArtworkKind::Album, album});
        ASSERT_TRUE(cover);
        covers.push_back(*cover);
    }
    // With the sources unreadable, the thumbnails still come from the cache: nothing is read from them again.
    std::filesystem::rename(folder / "cover.jpg", dir / "moved.jpg");
    std::filesystem::rename(song, dir / "moved.flac");
    for (size_t i = 0; i < albums.size(); ++i) {
        const auto again = cache.get(&store, {ArtworkKind::Album, albums[i]});
        ASSERT_TRUE(again);
        EXPECT_EQ(again->key, covers[i].key);
    }
    EXPECT_EQ(cache.prepare(store, [] { return true; }), 0u);
}
