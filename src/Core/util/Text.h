// UTF-8 / UTF-16 conversion and path helpers.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace camel {

std::string toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);

// Path with the \\?\ prefix so long and NAS paths open reliably (docs/計劃書.md §15.1).
std::wstring longPath(const std::filesystem::path& p);

} // namespace camel
