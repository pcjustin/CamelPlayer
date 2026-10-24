// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <functional>
#include "Server/Artwork/ArtworkProvider.h"
#include "Server/Library/Sqlite.h"

namespace camel::server::library {

struct FileVersion {
    int64_t size = 0, mtime = 0, volume = 0;
    std::vector<uint8_t> index;
    int64_t created = 0;
    std::string volumeKey;
    std::vector<uint8_t> legacyIndex;
    bool operator==(const FileVersion& other) const;
    bool sameFile(const FileVersion& other) const;
};

struct DirectoryEntry {
    std::wstring name;
    uint32_t attributes = 0;
    FileVersion version; // files only
};

FileVersion fileVersion(const std::filesystem::path& path);
// Enumerates an open directory handle with its file IDs; a null result requests FindFirstFileExW and per-file queries.
std::optional<std::vector<DirectoryEntry>> directoryEntries(void* directory, const CancellationToken& token);
std::wstring storageKey(const std::filesystem::path& path);
// Columns: size, mtime, volume_serial, file_index, created, volume_key.
FileVersion storedVersion(const sqlite::Statement& row, int column);
std::optional<std::vector<uint8_t>> fileDigest(const std::filesystem::path& path, const FileVersion& expected,
                                               const CancellationToken& token, const std::function<bool()>& playbackActive);

// Reconciles only newly published files after a complete scan; retains the original identity and its references.
size_t reconcileMoves(sqlite::Database& db, const std::vector<int64_t>& added);

} // namespace camel::server::library
