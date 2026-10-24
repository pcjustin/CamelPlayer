// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Artwork providers (docs/Plan.md §7.4, docs/Extension-points.md #3): folder images by name and extension rank, the
// parent folder for disc subfolders, network providers only with consent, failures and cancellation.
#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/util/Text.h"
#include "Server/Artwork/ArtworkProvider.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

std::filesystem::path freshDir(const char* name) {
    const auto dir = testutil::tempDir() / "artwork" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string text(const ArtworkCandidate& c) {
    return {c.bytes.begin(), c.bytes.end()};
}

class FakeProvider final : public IArtworkProvider {
public:
    FakeProvider(std::string id, bool network, bool fail = false) : id_(std::move(id)), network_(network), fail_(fail) {}
    std::string id() const override { return id_; }
    bool needsNetwork() const override { return network_; }
    std::vector<ArtworkCandidate> find(const ArtworkQuery&, const CancellationToken&) override {
        ++calls;
        if (fail_) throw std::runtime_error("provider down");
        ArtworkCandidate c;
        c.mime = "image/png";
        c.source = id_;
        return {c};
    }
    int calls = 0;

private:
    std::string id_;
    bool network_, fail_;
};

} // namespace

TEST(ArtworkProvider, FolderImagePrefersNameThenExtensionIgnoringCase) {
    const auto dir = freshDir("ranked");
    writeBytes(dir / "front.jpg", "front");
    writeBytes(dir / "Folder.PNG", "folder-png");
    writeBytes(dir / "folder.jpg", "folder-jpg");
    writeBytes(dir / "cover.txt", "not an image");
    writeBytes(dir / "back.jpg", "back");
    auto provider = makeFolderArtworkProvider();
    ArtworkQuery query;
    query.albumFolder = dir;
    auto found = provider->find(query, {});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(text(found[0]), "folder-jpg");
    EXPECT_EQ(found[0].mime, "image/jpeg");
    EXPECT_EQ(found[0].source, "folder");
    EXPECT_EQ(found[0].file, dir / "folder.jpg");

    writeBytes(dir / "COVER.webp", "cover-webp");
    found = provider->find(query, {});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(text(found[0]), "cover-webp");
    EXPECT_EQ(found[0].mime, "image/webp");
}

TEST(ArtworkProvider, ChainFallsBackToTheParentOnlyForDiscFolders) {
    const auto album = freshDir("multidisc");
    writeBytes(album / "cover.jpeg", "album-cover");
    std::filesystem::create_directories(album / "CD2");
    std::filesystem::create_directories(album / "Bonus");
    const auto chain = localArtworkChain();
    EXPECT_EQ(chain.ids(), (std::vector<std::string>{"folder", "embedded", "parent-folder"}));
    ArtworkQuery query;
    query.albumFolder = album / "CD2";
    auto found = chain.first(query, {}, false);
    ASSERT_TRUE(found);
    EXPECT_EQ(text(*found), "album-cover");
    EXPECT_EQ(found->source, "parent-folder");

    query.albumFolder = album / "Bonus";
    EXPECT_FALSE(chain.first(query, {}, false));
    writeBytes(album / "CD2" / "cover.png", "disc-cover");
    query.albumFolder = album / "CD2";
    found = chain.first(query, {}, false);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->source, "folder");
    EXPECT_EQ(text(*found), "disc-cover");
}

TEST(ArtworkProvider, EmptyMissingAndOversizedImagesAreSkipped) {
    const auto dir = freshDir("bad");
    writeBytes(dir / "cover.jpg", "");
    ArtworkQuery query;
    query.albumFolder = dir;
    EXPECT_TRUE(makeFolderArtworkProvider()->find(query, {}).empty());
    {
        std::ofstream big(dir / "cover.jpg", std::ios::binary);
        big.seekp(std::streamoff(kMaxArtworkBytes));
        big.put('x');
    }
    EXPECT_TRUE(makeFolderArtworkProvider()->find(query, {}).empty());
    query.albumFolder = dir / "missing";
    EXPECT_TRUE(makeFolderArtworkProvider()->find(query, {}).empty());
}

TEST(ArtworkProvider, NetworkProvidersRunOnlyWhenAllowedAndFailuresAreSkipped) {
    ArtworkChain chain;
    auto failing = std::make_unique<FakeProvider>("broken", false, true);
    auto online = std::make_unique<FakeProvider>("online", true);
    auto* failingRaw = failing.get();
    auto* onlineRaw = online.get();
    chain.add(std::move(failing));
    chain.add(std::move(online));
    EXPECT_FALSE(chain.first({}, {}, false));
    EXPECT_EQ(onlineRaw->calls, 0);
    EXPECT_EQ(failingRaw->calls, 1);
    const auto found = chain.first({}, {}, true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->source, "online");

    CancellationToken token;
    token.cancel();
    EXPECT_FALSE(chain.first({}, token, true));
    EXPECT_EQ(onlineRaw->calls, 1);
}

TEST(ArtworkProvider, DiscFolderNames) {
    for (const wchar_t* name : {L"CD1", L"cd 2", L"Disc 03", L"disk2", L"Album (CD 1)", L"碟1", L"碟 02", L"[Disc1]"})
        EXPECT_TRUE(isDiscFolderName(name)) << camel::toUtf8(name);
    for (const wchar_t* name : {L"ABCD1", L"Discography", L"Bonus", L"CD", L"碟", L"Disco 2", L"Bucd 3"})
        EXPECT_FALSE(isDiscFolderName(name)) << camel::toUtf8(name);
}

TEST(ArtworkProvider, ChildCancellationDoesNotCancelItsParentOrSiblings) {
    CancellationToken parent;
    auto first = parent.child(), second = parent.child();
    first.cancel();
    EXPECT_TRUE(first.cancelled());
    EXPECT_FALSE(parent.cancelled());
    EXPECT_FALSE(second.cancelled());
    parent.cancel();
    EXPECT_TRUE(second.cancelled());
    EXPECT_TRUE(second.child().cancelled());
}

TEST(ArtworkProvider, AlbumDiscSuffixIsRemovedOnlyAtEnd) {
    EXPECT_EQ(albumTitleWithoutDiscSuffix(L"Album CD1"), L"Album");
    EXPECT_EQ(albumTitleWithoutDiscSuffix(L"Album [Disc 2]"), L"Album");
    EXPECT_EQ(albumTitleWithoutDiscSuffix(L"Album (碟3)"), L"Album");
    EXPECT_EQ(albumTitleWithoutDiscSuffix(L"Album Discography"), L"Album Discography");
    EXPECT_EQ(albumTitleWithoutDiscSuffix(L"CD1 Album"), L"CD1 Album");
}
