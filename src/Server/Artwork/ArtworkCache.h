// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <functional>
#include <mutex>
#include <map>
#include "Server/Library/LibraryStore.h"

namespace camel::server {
enum class ArtworkKind { Album, Folder, Track };
struct ArtworkRequest {
    ArtworkKind kind = ArtworkKind::Album;
    int64_t id = 0;
    std::string path; // Track only: the current playback source, including files outside the library
    unsigned pixels = 256;
};
struct ArtworkReference {
    std::string key;
    std::filesystem::path path; // generated local thumbnail, never a music-library file
};
class ArtworkCache {
public:
    explicit ArtworkCache(std::filesystem::path directory, uintmax_t maxBytes = 512ull << 20);
    std::optional<ArtworkReference> get(const LibraryStore* store, const ArtworkRequest& request,
                                        const CancellationToken& token = {});
    std::optional<ArtworkReference> thumbnail(const ArtworkCandidate& image, unsigned pixels,
                                              const CancellationToken& token = {});
    // Settings › Clear cover cache: deletes every generated thumbnail; later requests generate them again.
    void clear();
    // Makes the album-wall thumbnails the library lacks, until `yield` asks for the storage; returns how many it made.
    size_t prepare(const LibraryStore& store, const std::function<bool()>& yield, const CancellationToken& token = {});

private:
    // A cover the scan chose for an album or folder; its key comes from the stored file version, so an existing thumbnail
    // is found without touching the music storage. nullopt when the library records no such cover.
    std::optional<ArtworkReference> stored(const LibraryStore& store, const ArtworkRequest& request,
                                           const CancellationToken& token);
    // A non-empty `key` names the thumbnail instead of a digest of the source.
    std::optional<ArtworkReference> generate(const ArtworkCandidate& image, unsigned pixels, const CancellationToken& token,
                                             const std::string& key = {});
    void trim();
    std::filesystem::path directory_;
    uintmax_t maxBytes_;
    std::optional<uintmax_t> diskBytes_;
    std::map<std::string, std::optional<ArtworkReference>> embedded_;
    std::mutex mutex_;
};
} // namespace camel::server
