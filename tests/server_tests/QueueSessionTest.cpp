// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <filesystem>

#include "Common/AudioFormat.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Playback/QueueSession.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;

// The format the playback bar showed comes back with the session for every format, those after DSD among them.
TEST(QueueSession, EveryFormatComesBackWithTheSession) {
    const auto dir = testutil::tempDir() / "queue-session-formats";
    std::filesystem::remove_all(dir);
    LibraryStore store(dir);
    for (const auto format : audio::kAllFormats) {
        SavedSession session;
        session.track.format = format;
        store.write([&](sqlite::Database& db) { saveSession(db, session, {}); }).get();
        auto db = store.openReader();
        const auto loaded = loadSession(db);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->track.format, format) << audio::formatId(format);
    }
}
