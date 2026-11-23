// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace camel::server {

// The tokens this Control point was given by the Servers it paired with (docs/Plan.md §17.8), by each Server's ID, kept
// in one file encrypted with DPAPI for the Windows user. A file it cannot read is taken as empty, so pairing again
// mends it.
class ServerTokens {
public:
    explicit ServerTokens(std::filesystem::path file);

    std::optional<std::string> token(const std::string& server) const;
    void set(const std::string& server, const std::string& token);
    void forget(const std::string& server);

private:
    void save() const;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> tokens_;
};

} // namespace camel::server
