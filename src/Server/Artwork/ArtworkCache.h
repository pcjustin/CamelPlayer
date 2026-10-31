// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <functional>
#include <mutex>
#include <map>
#include "Server/Library/LibraryStore.h"

namespace camel::server {
enum class ArtworkKind { Album, Folder, Track, Artist };
struct ArtworkRequest {
    ArtworkKind kind = ArtworkKind::Album;
    int64_t id = 0;
    std::string path; // Track only: the current playback source, including files outside the library
    std::string name; // Artist only: an artist's or composer's name
};
struct ArtworkReference {
    std::string key;
    std::filesystem::path path; // generated local thumbnail, never a music-library file
};
// A square JPEG of at most `pixels` from an image file's bytes: the middle of a wide picture, the top of a tall one.
// Throws std::invalid_argument when the bytes are not a picture it can read.
std::vector<uint8_t> squarePicture(const std::vector<uint8_t>& image, unsigned pixels);

class ArtworkCache {
public:
    // Every view shows covers from one size: at most 160 view pixels (the album details) up to 160 % scaling.
    static constexpr unsigned kPixels = 256;

    explicit ArtworkCache(std::filesystem::path directory, uintmax_t maxBytes = 512ull << 20);
    std::optional<ArtworkReference> get(const LibraryStore* store, const ArtworkRequest& request,
                                        const CancellationToken& token = {});
    std::optional<ArtworkReference> thumbnail(const ArtworkCandidate& image, const CancellationToken& token = {});
    // Settings › Clear cover cache: deletes every generated thumbnail; later requests generate them again.
    void clear();
    // Makes the album thumbnails the library lacks, until `yield` asks for the storage; returns how many it made.
    size_t prepare(const LibraryStore& store, const std::function<bool()>& yield, const CancellationToken& token = {});

private:
    // A cover the scan chose for an album or folder; its key comes from the stored file version, so an existing thumbnail
    // is found without touching the music storage. nullopt when the library records no such cover.
    std::optional<ArtworkReference> stored(const LibraryStore& store, const ArtworkRequest& request,
                                           const CancellationToken& token);
    std::optional<ArtworkReference> artist(const LibraryStore& store, const std::string& name, const CancellationToken& token);
    // A non-empty `key` names the thumbnail instead of a digest of the source.
    std::optional<ArtworkReference> generate(const ArtworkCandidate& image, const CancellationToken& token,
                                             const std::string& key = {});
    void trim();
    std::filesystem::path directory_;
    uintmax_t maxBytes_;
    std::optional<uintmax_t> diskBytes_;
    std::map<std::string, std::optional<ArtworkReference>> embedded_;
    std::mutex mutex_;
};
} // namespace camel::server
