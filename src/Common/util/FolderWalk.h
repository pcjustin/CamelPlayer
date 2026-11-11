// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace camel {

// Removes every entry of `folder`; returns the names of those that could not be removed. Never throws: the walk ends
// where the folder can no longer be read, rather than taking the program with it.
inline std::vector<std::wstring> removeEntries(const std::filesystem::path& folder) {
    std::vector<std::wstring> kept;
    std::error_code error;
    std::filesystem::directory_iterator entry(folder, error);
    for (const std::filesystem::directory_iterator end; !error && entry != end; entry.increment(error)) {
        std::error_code removing;
        std::filesystem::remove_all(entry->path(), removing);
        if (removing) kept.push_back(entry->path().filename().wstring());
    }
    return kept;
}

// The size of the file at `path`, or of every file under the folder it names that can be read. Never throws.
inline uintmax_t sizeOnDisk(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        const auto size = std::filesystem::file_size(path, error);
        return error ? 0 : size;
    }
    uintmax_t total = 0;
    std::filesystem::recursive_directory_iterator entry(path, std::filesystem::directory_options::skip_permission_denied, error);
    for (const std::filesystem::recursive_directory_iterator end; !error && entry != end; entry.increment(error)) {
        std::error_code reading;
        if (!entry->is_regular_file(reading)) continue;
        const auto size = entry->file_size(reading);
        if (!reading) total += size;
    }
    return total;
}

} // namespace camel
