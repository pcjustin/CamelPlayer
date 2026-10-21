// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// UTF-8 / UTF-16 conversion and path helpers.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace camel {

std::string toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);

// Path with the \\?\ prefix so long and NAS paths open reliably (docs/Plan.md §15.1).
std::wstring longPath(const std::filesystem::path& p);

} // namespace camel
