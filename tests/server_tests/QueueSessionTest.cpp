// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Common/AudioFormat.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Playback/QueueSession.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;

namespace {
const std::string kZone = "wasapi:";
}

// The format the playback bar showed comes back with the session for every format, those after DSD among them.
TEST(QueueSession, EveryFormatComesBackWithTheSession) {
    const auto dir = testutil::tempDir() / "queue-session-formats";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    for (const auto format : audio::kAllFormats) {
        SavedSession session;
        session.track.format = format;
        store.write([&](sqlite::Database& db) { saveSession(db, kZone, session, {}); }).get();
        auto db = store.openReader();
        const auto loaded = loadSession(db, kZone);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->track.format, format) << audio::formatId(format);
    }
}

namespace {

SavedQueueEntry entryNamed(const std::string& name) {
    return {TrackSource("C:\\Music\\" + name + ".flac"), 0, name, false};
}

std::vector<std::string> pathsOf(LibraryStore& store, const std::string& zone = kZone) {
    auto db = store.openReader();
    std::vector<std::string> paths;
    if (const auto loaded = loadSession(db, zone))
        for (const auto& entry : loaded->entries) paths.push_back(entry.source.pathUtf8());
    return paths;
}

} // namespace

// Entries before the first one that changed are left as stored; those after it are written again.
TEST(QueueSession, OnlyTheEntriesFromTheFirstChangedAreWrittenAgain) {
    const auto dir = testutil::tempDir() / "queue-session-kept";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    SavedSession session;
    for (const auto* name : {"a", "b", "c"}) session.entries.push_back(entryNamed(name));
    const auto save = [&](size_t kept) {
        store.write([&](sqlite::Database& db) { saveSession(db, kZone, session, {true, false, kept}); }).get();
    };
    save(0);
    session.entries.push_back(entryNamed("d"));
    save(3);
    EXPECT_EQ(pathsOf(store),
              (std::vector<std::string>{"C:\\Music\\a.flac", "C:\\Music\\b.flac", "C:\\Music\\c.flac", "C:\\Music\\d.flac"}));
    session.entries.erase(session.entries.begin() + 2, session.entries.end());
    session.entries[1] = entryNamed("e");
    save(1);
    EXPECT_EQ(pathsOf(store), (std::vector<std::string>{"C:\\Music\\a.flac", "C:\\Music\\e.flac"}));
}

// A scan deletes the rows of files that are gone: the entries kept are written again rather than lost.
TEST(QueueSession, EntriesWhoseRowsAScanDeletedAreWrittenAgain) {
    const auto dir = testutil::tempDir() / "queue-session-holes";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    SavedSession session;
    for (const auto* name : {"a", "b", "c"}) session.entries.push_back(entryNamed(name));
    store.write([&](sqlite::Database& db) { saveSession(db, kZone, session, {true, false}); }).get();
    store.write([](sqlite::Database& db) { db.exec("DELETE FROM queue WHERE position=1"); }).get();
    session.entries.push_back(entryNamed("d"));
    store.write([&](sqlite::Database& db) { saveSession(db, kZone, session, {true, false, 3}); }).get();
    EXPECT_EQ(pathsOf(store),
              (std::vector<std::string>{"C:\\Music\\a.flac", "C:\\Music\\b.flac", "C:\\Music\\c.flac", "C:\\Music\\d.flac"}));
}

// Zones (docs/Plan.md §5.12): each zone's queue is its own, and a zone's writes leave another's rows alone.
TEST(QueueSession, EachZoneKeepsItsOwnQueue) {
    const auto dir = testutil::tempDir() / "queue-session-zones";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    SavedSession one, two;
    for (const auto* name : {"a", "b", "c"}) one.entries.push_back(entryNamed(name));
    two.entries.push_back(entryNamed("x"));
    store.write([&](sqlite::Database& db) { saveSession(db, "asio:1", one, {true, true}); }).get();
    store.write([&](sqlite::Database& db) { saveSession(db, "ks:2", two, {true, true}); }).get();
    two.entries.clear();
    store.write([&](sqlite::Database& db) { saveSession(db, "ks:2", two, {true, true}); }).get();
    EXPECT_EQ(pathsOf(store, "asio:1"),
              (std::vector<std::string>{"C:\\Music\\a.flac", "C:\\Music\\b.flac", "C:\\Music\\c.flac"}));
    EXPECT_TRUE(pathsOf(store, "ks:2").empty());
    auto db = store.openReader();
    EXPECT_TRUE(hasSavedSession(db, "ks:2"));
    EXPECT_FALSE(hasSavedSession(db, "wasapi:"));
}

TEST(QueueSession, AZonesQueueCanBeDeletedOrGivenToAnother) {
    const auto dir = testutil::tempDir() / "queue-session-move";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    SavedSession one, two;
    one.entries.push_back(entryNamed("a"));
    two.entries.push_back(entryNamed("x"));
    store
        .write([&](sqlite::Database& db) {
            saveSession(db, "asio:1", one, {true, true});
            saveSession(db, "ks:2", two, {true, true});
        })
        .get();
    store.write([](sqlite::Database& db) { moveSession(db, "asio:1", "ks:2"); }).get();
    EXPECT_EQ(pathsOf(store, "ks:2"), (std::vector<std::string>{"C:\\Music\\a.flac"}));
    {
        auto db = store.openReader();
        EXPECT_FALSE(hasSavedSession(db, "asio:1"));
    }
    store.write([](sqlite::Database& db) { deleteSession(db, "ks:2"); }).get();
    auto db = store.openReader();
    EXPECT_FALSE(hasSavedSession(db));
    EXPECT_EQ(db.scalar("SELECT count(*) FROM queue"), 0);
}
