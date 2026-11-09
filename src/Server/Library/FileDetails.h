// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A track's file details (docs/Task-breakdown.md FD-R01): every text tag under each tag format the file carries, and its
// technical data, read when asked; pictures, lyrics and binary tags are left out, and nothing is written.
#pragma once

#include <filesystem>
#include <string>

#include "Server/Api/ServerApi.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {

// `db` may be null: the file is then read without the library's fields to compare with.
FileDetails readFileDetails(const std::string& pathUtf8, sqlite::Database* db);

} // namespace camel::server
