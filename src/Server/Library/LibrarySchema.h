// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// library.db schema and its migrations, versioned by PRAGMA user_version (docs/Plan.md §7.6).
#pragma once
#include <functional>
#include <vector>

#include "Server/Library/Sqlite.h"

namespace camel::server {

// Migration i moves the schema from version i to i + 1.
using Migration = std::function<void(sqlite::Database&)>;
const std::vector<Migration>& libraryMigrations();

// Applies the migrations after the database's version, each in its own transaction together with its user_version.
// Throws std::runtime_error when the database is newer than `migrations` knows.
void migrate(sqlite::Database& db, const std::vector<Migration>& migrations);

} // namespace camel::server
