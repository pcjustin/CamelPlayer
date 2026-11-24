// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <vector>

#include "Server/Api/ServerApi.h"

namespace camel::server {

// What IServer::serverFolders lists on this computer: its drives for an empty path, the disk shares of `\\computer`,
// else the folders in the path that are neither hidden nor system ones, by name. Throws std::runtime_error for a
// path that cannot be listed.
std::vector<ServerFolder> foldersIn(const std::string& pathUtf8);

} // namespace camel::server
