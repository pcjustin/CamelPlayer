// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include "Server/Library/Sqlite.h"
#include "TestUtil.h"

// A default-constructed view has no data: it still binds the empty string, not SQL NULL.
TEST(Sqlite, AnEmptyViewBindsAnEmptyString) {
    const auto file = testutil::tempDir() / "sqlite-empty-view.db";
    std::filesystem::remove(file);
    {
        camel::server::sqlite::Database db(file);
        auto row = db.prepare("SELECT ?1 IS NULL, ?1 = ''");
        row.bind(1, std::string_view{});
        ASSERT_TRUE(row.step());
        EXPECT_EQ(row.integer(0), 0) << "bound as NULL";
        EXPECT_EQ(row.integer(1), 1);
    }
    std::filesystem::remove(file);
}
