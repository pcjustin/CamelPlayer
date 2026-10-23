// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>
#include <stdexcept>
#include <future>
#include "Server/Library/LibraryStore.h"
#include "Server/Library/LibraryBrowse.h"
#include "TestUtil.h"
#include "Server/Library/LibraryText.h"

using namespace camel::server;

TEST(LibraryText, MatchesTraditionalSimplifiedKanaWidthAndCase) {
    EXPECT_EQ(normalizeLibrarySearch("臺灣音樂 ＡＢＣ"), normalizeLibrarySearch("台湾音乐 abc"));
    EXPECT_EQ(normalizeLibrarySearch("ｶﾞﾗｽの花"), normalizeLibrarySearch("がらすの花"));
    EXPECT_EQ(normalizeLibrarySearch("カタカナ"), normalizeLibrarySearch("かたかな"));
    EXPECT_EQ(normalizeLibrarySearch("Café"), normalizeLibrarySearch("Cafe\xcc\x81"));
    EXPECT_TRUE(normalizeLibrarySearch("").empty());
}

TEST(LibraryText, SupportsStrokeAndBopomofoWithVersionedKeys) {
    const auto stroke = librarySortKey("一", L"zh-TW");
    const auto phonetic = librarySortKey("一", L"zh-TW_pronun");
    EXPECT_FALSE(stroke.empty());
    EXPECT_FALSE(phonetic.empty());
    EXPECT_FALSE(libraryNlsVersion(L"zh-TW").empty());
    EXPECT_FALSE(libraryNlsVersion(L"zh-TW_pronun").empty());
    EXPECT_EQ(stroke, librarySortKey("一", L"zh-TW"));
    EXPECT_LT(stroke, librarySortKey("阿", L"zh-TW"));
    EXPECT_LT(librarySortKey("阿", L"zh-TW_pronun"), phonetic);
    EXPECT_THROW(librarySortKey("text", L"not-a-locale"), std::invalid_argument);
}

TEST(LibraryText, RebuildsOnNlsVersionAndLocaleChangesWhileReadersRemainAvailable) {
    const auto directory = testutil::tempDir() / "library-r08-rebuild";
    std::filesystem::remove_all(directory);
    LibraryStore store(directory);
    store.write([](sqlite::Database& db) {
        db.exec("INSERT INTO roots(id,path,added) VALUES(1,'C:/music',0);"
                "INSERT INTO folders(id,root_id,path) VALUES(1,1,'C:/music/album');"
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<201) "
                "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) SELECT x,1,1,'C:/music/'||x||'.flac','audio',1,1 FROM n;"
                "INSERT INTO tracks(id,file_id,title,artist,album,title_sort_tag) "
                "SELECT id,id,'臺灣音樂','カタカナ','ＡＢＣ','zzz' FROM files;"
                "INSERT INTO track_values(track_id,field,position,value,sort_source) VALUES(1,'artist',0,'音樂','before');");
    }).get();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    {
        LibraryTextIndex index(store, [&](std::wstring_view) {
            entered.set_value();
            released.wait();
            return std::vector<uint8_t>{1};
        });
        const auto ready = entered.get_future().wait_for(std::chrono::seconds(10));
        EXPECT_EQ(ready, std::future_status::ready);
        auto reader = store.openReader();
        EXPECT_EQ(reader.scalar("SELECT count(*) FROM library_tracks"), 201);
        release.set_value();
        index.waitIdle();
        auto row = reader.prepare("SELECT title,title_norm,title_sort FROM tracks WHERE id=1");
        ASSERT_TRUE(row.step());
        EXPECT_EQ(row.text(0), "臺灣音樂");
        EXPECT_EQ(row.text(1), "台湾音乐");
        EXPECT_EQ(row.blob(2), librarySortKey("zzz"));
        EXPECT_EQ(searchTracks(store, "台湾音乐", 0, 10).total, 201);
        EXPECT_EQ(searchTracks(store, "音樂", 0, 10).total, 201);
        EXPECT_EQ(searchTracks(store, "樂", 0, 10).total, 201);
        EXPECT_EQ(searchTracks(store, "ｶﾀｶﾅ", 0, 10).total, 201);
        EXPECT_EQ(searchTracks(store, "ab", 0, 10).total, 201);
    }
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title_sort=X'01'"); }).get();
    {
        LibraryTextIndex index(store, [](std::wstring_view) { return std::vector<uint8_t>{2}; });
        index.waitIdle();
        {
            auto reader = store.openReader();
            EXPECT_EQ(reader.scalar("SELECT count(*) FROM tracks WHERE length(title_sort)>1"), 201);
            auto value = reader.prepare("SELECT value_norm,sort_key FROM track_values WHERE track_id=1");
            ASSERT_TRUE(value.step());
            EXPECT_EQ(value.text(0), "音乐");
            EXPECT_EQ(value.blob(1), librarySortKey("before"));
        }
        index.setLocale(L"zh-TW_pronun");
        index.waitIdle();
        auto reader = store.openReader();
        auto row = reader.prepare("SELECT title_sort FROM tracks WHERE id=1");
        ASSERT_TRUE(row.step());
        EXPECT_EQ(row.blob(0), librarySortKey("zzz", L"zh-TW_pronun"));
        auto locale = reader.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
        ASSERT_TRUE(locale.step());
        EXPECT_EQ(locale.text(0), "zh-TW_pronun");
    }
}
