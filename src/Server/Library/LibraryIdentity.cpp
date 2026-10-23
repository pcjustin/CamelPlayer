// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibraryIdentity.h"

#include <windows.h>
#include <bcrypt.h>
#include <winnetwk.h>
#include <wil/resource.h>

#include <iterator>
#include <cstring>
#include <set>

#include "Common/util/Text.h"
#include "Server/Library/LibraryStore.h"

#pragma comment(lib, "mpr.lib")

namespace camel::server::library {
namespace {

wil::unique_handle open(const std::filesystem::path& path, DWORD access = FILE_READ_ATTRIBUTES,
                        DWORD flags = FILE_FLAG_BACKUP_SEMANTICS) {
    return wil::unique_handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

FileVersion version(HANDLE handle, const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) throw std::runtime_error("cannot read file identity");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("reparse point is not a file identity");
    const uint64_t index = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    FileVersion out{int64_t((uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow),
                    int64_t((uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime),
                    info.dwVolumeSerialNumber,
                    {},
                    int64_t((uint64_t(info.ftCreationTime.dwHighDateTime) << 32) | info.ftCreationTime.dwLowDateTime)};
    out.legacyIndex.resize(sizeof(index));
    memcpy(out.legacyIndex.data(), &index, sizeof(index));
    out.index = out.legacyIndex;
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        out.index.assign(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier));
        if (!isNetworkPath(path)) {
            wchar_t mount[32768]{}, guid[64]{}, filesystem[32]{};
            if (GetVolumePathNameW(path.c_str(), mount, DWORD(std::size(mount))) &&
                GetVolumeNameForVolumeMountPointW(mount, guid, DWORD(std::size(guid))) &&
                GetVolumeInformationW(mount, nullptr, 0, nullptr, nullptr, nullptr, filesystem, DWORD(std::size(filesystem))) &&
                (_wcsicmp(filesystem, L"NTFS") == 0 || _wcsicmp(filesystem, L"ReFS") == 0))
                out.volumeKey = toUtf8(guid);
        }
    }
    return out;
}

bool sourceGone(const std::string& source, const std::string& destination, const std::string& root, const FileVersion& prior) {
    const auto rootPath = std::filesystem::path(toWide(root));
    auto rootHandle = open(rootPath, FILE_LIST_DIRECTORY);
    if (!rootHandle) return false;
    wchar_t resolved[32768]{};
    const DWORD length =
        GetFinalPathNameByHandleW(rootHandle.get(), resolved, DWORD(std::size(resolved)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= std::size(resolved) || _wcsicmp(rootPath.c_str(), resolved) != 0) return false;
    BY_HANDLE_FILE_INFORMATION rootInfo{};
    if (!GetFileInformationByHandle(rootHandle.get(), &rootInfo) || !(rootInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (rootInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    if (source == destination) return true;
    const auto sourcePath = std::filesystem::path(toWide(source));
    auto handle = open(sourcePath);
    if (handle) {
        if (prior.volumeKey.empty()) return false;
        return !version(handle.get(), sourcePath).sameFile(prior);
    }
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

constexpr const char* kMetadata =
    "title,album,album_artist,artist,composer,genre,year,track_number,track_total,disc_number,disc_total,"
    "format,sample_rate,bits_per_sample,channels,total_frames,embedded_cover,lyrics,"
    "rg_track_gain,rg_track_peak,rg_album_gain,rg_album_peak,"
    "title_norm,album_norm,artist_norm,title_sort,album_sort,artist_sort,title_sort_tag,album_sort_tag,artist_sort_tag";

void merge(sqlite::Database& db, int64_t prior, int64_t current) {
    db.prepare("DELETE FROM tracks WHERE file_id=? AND cue_index=0 "
               "AND NOT EXISTS(SELECT 1 FROM tracks WHERE file_id=? AND cue_index=0)")
        .bind(1, prior)
        .bind(2, current)
        .run();
    db.prepare(std::string("UPDATE tracks SET (") + kMetadata + ")=(SELECT " + kMetadata +
               " FROM tracks WHERE file_id=? AND cue_index=0) WHERE file_id=? AND cue_index=0 "
               "AND EXISTS(SELECT 1 FROM tracks WHERE file_id=? AND cue_index=0)")
        .bind(1, current)
        .bind(2, prior)
        .bind(3, current)
        .run();
    auto oldTrack = db.prepare("SELECT id FROM tracks WHERE file_id=? AND cue_index=0");
    auto newTrack = db.prepare("SELECT id FROM tracks WHERE file_id=? AND cue_index=0");
    if (oldTrack.bind(1, prior).step() && newTrack.bind(1, current).step()) {
        db.prepare("DELETE FROM track_values WHERE track_id=?").bind(1, oldTrack.integer(0)).run();
        db.prepare("INSERT INTO track_values(track_id,field,position,value,value_norm,sort_key,sort_source) "
                   "SELECT ?,field,position,value,value_norm,sort_key,sort_source FROM track_values WHERE track_id=?")
            .bind(1, oldTrack.integer(0))
            .bind(2, newTrack.integer(0))
            .run();
    }
    db.prepare("UPDATE tracks SET file_id=? WHERE file_id=? AND cue_index NOT IN "
               "(SELECT cue_index FROM tracks WHERE file_id=?)")
        .bind(1, prior)
        .bind(2, current)
        .bind(3, prior)
        .run();
    db.prepare("DELETE FROM tracks WHERE file_id=?").bind(1, current).run();
    for (const auto* table : {"covers", "playlist_items", "queue"})
        db.prepare(std::string("UPDATE ") + table + " SET file_id=? WHERE file_id=?").bind(1, prior).bind(2, current).run();
    db.prepare("UPDATE tracks SET cue_file_id=? WHERE cue_file_id=?").bind(1, prior).bind(2, current).run();
    db.prepare("UPDATE files SET available=0 WHERE id=?").bind(1, current).run();
    db.prepare("UPDATE files SET (folder_id,root_id,path,kind,size,mtime,volume_serial,file_index,created,volume_key,last_seen)="
               "(SELECT folder_id,root_id,path,kind,size,mtime,volume_serial,file_index,created,volume_key,last_seen "
               "FROM files WHERE id=?),content_hash=CASE WHEN size=(SELECT size FROM files WHERE id=?) "
               "AND mtime=(SELECT mtime FROM files WHERE id=?) THEN COALESCE((SELECT content_hash FROM files WHERE "
               "id=?),content_hash) "
               "ELSE (SELECT content_hash FROM files WHERE id=?) END,available=1 WHERE id=?")
        .bind(1, current)
        .bind(2, current)
        .bind(3, current)
        .bind(4, current)
        .bind(5, current)
        .bind(6, prior)
        .run();
    db.prepare("UPDATE queue SET path=(SELECT path FROM files WHERE id=?) WHERE file_id=?").bind(1, prior).bind(2, prior).run();
    // Keep the provisional tombstone, but exclude it from future identity matches.
    db.prepare("UPDATE files SET content_hash=NULL,volume_key=NULL,file_index=NULL,identity_state=2 WHERE id=?")
        .bind(1, current)
        .run();
}

} // namespace

bool FileVersion::operator==(const FileVersion& other) const {
    return size == other.size && mtime == other.mtime && volume == other.volume && index == other.index &&
           created == other.created && volumeKey == other.volumeKey;
}

bool FileVersion::sameFile(const FileVersion& other) const {
    if (other.created == 0 && other.index.size() == 8) return volume == other.volume && legacyIndex == other.index;
    return volume == other.volume && index == other.index && created == other.created && volumeKey == other.volumeKey;
}

FileVersion fileVersion(const std::filesystem::path& path) {
    auto handle = open(path);
    if (!handle) throw std::runtime_error("cannot read file identity: " + toUtf8(path.wstring()));
    return version(handle.get(), path);
}

std::wstring storageKey(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::wstring text = path.wstring();
    if (text.starts_with(L"\\\\?\\") && text.size() > 5 && text[5] == L':') text.erase(0, 4);
    if (text.size() > 2 && text[1] == L':' && GetDriveTypeW(text.substr(0, 3).c_str()) == DRIVE_REMOTE) {
        wchar_t remote[32768]{};
        DWORD size = DWORD(std::size(remote));
        if (WNetGetConnectionW(text.substr(0, 2).c_str(), remote, &size) != NO_ERROR) return {};
        text = remote + text.substr(2);
    }
    if (_wcsnicmp(text.c_str(), L"\\\\?\\UNC\\", 8) == 0) text = L"\\\\" + text.substr(8);
    if (text.starts_with(L"\\\\") && !text.starts_with(L"\\\\?\\")) {
        const auto server = text.find(L'\\', 2);
        if (server == std::wstring::npos) return {};
        const auto end = text.find(L'\\', server + 1);
        if (end != std::wstring::npos) text.resize(end);
    } else {
        wchar_t mount[32768]{}, guid[64]{};
        if (!GetVolumePathNameW(text.c_str(), mount, DWORD(std::size(mount))) ||
            !GetVolumeNameForVolumeMountPointW(mount, guid, DWORD(std::size(guid)))) return {};
        text = guid;
    }
    CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

FileVersion storedVersion(const sqlite::Statement& row, int column) {
    return {row.integer(column),  row.integer(column + 1), row.integer(column + 2),
            row.blob(column + 3), row.integer(column + 4), row.text(column + 5)};
}

std::optional<std::vector<uint8_t>> fileDigest(const std::filesystem::path& path, const FileVersion& expected,
                                               const CancellationToken& token, const std::function<bool()>& playbackActive) {
    if (token.cancelled() || (playbackActive && playbackActive())) return std::nullopt;
    auto handle = open(path, GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN);
    if (!handle || version(handle.get(), path) != expected) throw std::runtime_error("file changed before fingerprinting");
    wil::unique_bcrypt_algorithm algorithm;
    wil::unique_bcrypt_hash hash;
    if (BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm.get(), hash.put(), nullptr, 0, nullptr, 0, 0) != 0)
        throw std::runtime_error("cannot initialize SHA-256");
    std::vector<uint8_t> buffer(1 << 20);
    int64_t total = 0;
    for (;;) {
        if (token.cancelled() || (playbackActive && playbackActive())) return std::nullopt;
        DWORD bytes = 0;
        if (!ReadFile(handle.get(), buffer.data(), DWORD(buffer.size()), &bytes, nullptr))
            throw std::runtime_error("cannot read fingerprint source");
        if (!bytes) break;
        if (BCryptHashData(hash.get(), buffer.data(), bytes, 0) != 0) throw std::runtime_error("SHA-256 update failed");
        total += bytes;
    }
    if (total != expected.size || version(handle.get(), path) != expected || fileVersion(path) != expected)
        throw std::runtime_error("file changed during fingerprinting");
    if (token.cancelled() || (playbackActive && playbackActive())) return std::nullopt;
    std::vector<uint8_t> digest(32);
    if (BCryptFinishHash(hash.get(), digest.data(), DWORD(digest.size()), 0) != 0)
        throw std::runtime_error("SHA-256 finish failed");
    return digest;
}

size_t reconcileMoves(sqlite::Database& db, const std::vector<int64_t>& added) {
    std::set<int64_t> incoming;
    auto pending = db.prepare("SELECT id FROM files WHERE identity_state=1 AND available=1");
    while (pending.step()) incoming.insert(pending.integer(0));
    size_t moved = 0;
    for (const auto current : added) {
        auto next = db.prepare("SELECT path,kind,size,mtime,volume_serial,file_index,created,volume_key,content_hash "
                               "FROM files WHERE id=? AND available=1");
        if (!next.bind(1, current).step()) continue;
        auto stats = db.prepare("SELECT 1 FROM stats WHERE file_id=?");
        if (stats.bind(1, current).step()) continue;
        const auto destination = next.text(0), kind = next.text(1);
        const auto currentVersion = storedVersion(next, 2);
        std::vector<int64_t> candidates;
        if (!currentVersion.volumeKey.empty()) {
            auto matches = db.prepare("SELECT id FROM files WHERE volume_key=? AND file_index=? AND created=? AND kind=?");
            matches.bind(1, currentVersion.volumeKey).bind(2, currentVersion.index).bind(3, currentVersion.created).bind(4, kind);
            size_t arrivals = 0;
            while (matches.step()) {
                const int64_t id = matches.integer(0);
                arrivals += incoming.contains(id);
                if (!incoming.contains(id)) candidates.push_back(id);
            }
            if (arrivals != 1) candidates.clear();
        }
        if (candidates.empty() && next.blob(8).size() == 32) {
            auto matches = db.prepare("SELECT id FROM files WHERE content_hash=? AND kind=? AND size=?");
            matches.bind(1, next.blob(8)).bind(2, kind).bind(3, currentVersion.size);
            size_t arrivals = 0;
            while (matches.step()) {
                arrivals += incoming.contains(matches.integer(0));
                if (!incoming.contains(matches.integer(0))) candidates.push_back(matches.integer(0));
            }
            if (arrivals != 1) candidates.clear();
            auto unknown = db.prepare("SELECT 1 FROM files WHERE kind=? AND size=? AND content_hash IS NULL "
                                      "AND id<>? AND identity_state<>2 LIMIT 1");
            if (unknown.bind(1, kind).bind(2, currentVersion.size).bind(3, current).step()) candidates.clear();
        }
        if (candidates.size() != 1) continue;
        const int64_t prior = candidates.front();
        auto old = db.prepare("SELECT files.path,roots.path,files.size,files.mtime,volume_serial,file_index,created,volume_key "
                              "FROM files JOIN roots ON roots.id=files.root_id WHERE files.id=? AND roots.removed IS NULL");
        if (!old.bind(1, prior).step()) continue;
        try {
            if (fileVersion(std::filesystem::path(toWide(destination))) != currentVersion ||
                !sourceGone(old.text(0), destination, old.text(1), storedVersion(old, 2)))
                continue;
        } catch (const std::exception&) {
            continue;
        }
        merge(db, prior, current);
        moved++;
    }
    return moved;
}

} // namespace camel::server::library
