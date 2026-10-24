// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Artwork and metadata providers (docs/Extension-points.md #3, docs/Plan.md §7.4). Providers only return candidates;
// nothing here writes to music files (file writes go through the M7 edit queue).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace camel::server {

// Shared between the requester and the work; cancel() makes running providers stop at their next check.
class CancellationToken {
public:
    void cancel() { flag_->store(true, std::memory_order_release); }
    bool cancelled() const { return flag_->load(std::memory_order_acquire) || (parent_ && parent_->cancelled()); }
    CancellationToken child() const {
        CancellationToken token;
        token.parent_ = std::make_shared<CancellationToken>(*this);
        return token;
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<const CancellationToken> parent_;
};

struct ArtworkQuery {
    std::filesystem::path albumFolder; // the folder holding the album's tracks
    std::filesystem::path firstTrack;  // source of an embedded cover
    std::string albumArtist, album, artist, title;
    std::optional<std::string> musicbrainzReleaseId;
};

struct ArtworkCandidate {
    std::string mime;
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0; // 0 when the provider does not know
    std::string source;             // provider ID
    std::filesystem::path file;     // a local image's path; empty otherwise
};

class IArtworkProvider {
public:
    virtual ~IArtworkProvider() = default;
    virtual std::string id() const = 0;
    virtual bool needsNetwork() const = 0;
    virtual std::vector<ArtworkCandidate> find(const ArtworkQuery& query, const CancellationToken& token) = 0;
};

struct MetadataCandidate {
    std::vector<std::pair<std::string, std::vector<std::string>>> fields; // tag name → values
    std::string source;
};

// Results form a separate layer below the file's own tags (docs/Plan.md §17.3).
class IMetadataProvider {
public:
    virtual ~IMetadataProvider() = default;
    virtual std::string id() const = 0;
    virtual bool needsNetwork() const = 0;
    virtual std::vector<MetadataCandidate> find(const ArtworkQuery& query, const CancellationToken& token) = 0;
};

// Providers in priority order; the first that returns a candidate wins. Network providers run only when allowed (the
// first-run consent); a failing provider is skipped.
class ArtworkChain {
public:
    void add(std::unique_ptr<IArtworkProvider> provider);
    std::optional<ArtworkCandidate> first(const ArtworkQuery& query, const CancellationToken& token, bool allowNetwork) const;
    std::vector<std::string> ids() const;

private:
    std::vector<std::unique_ptr<IArtworkProvider>> providers_;
};

// `cover|folder|front.(jpg|jpeg|png|webp)` in the album folder, in that name and extension order, case-insensitive.
std::unique_ptr<IArtworkProvider> makeFolderArtworkProvider();
// The preference of a folder cover file name (lower wins) and its MIME type; nothing for any other file.
std::optional<std::pair<size_t, const char*>> folderArtworkRank(const std::filesystem::path& file);
// Locate the selected image without reading its payload.
std::optional<ArtworkCandidate> findFolderArtworkFile(const std::filesystem::path& folder, const CancellationToken& token);
// The same names in the parent folder when the album folder is a disc subfolder (CD1, Disc 2, 碟 3).
std::unique_ptr<IArtworkProvider> makeParentFolderArtworkProvider();
std::unique_ptr<IArtworkProvider> makeEmbeddedArtworkProvider();
// Local providers in the §7.4 order; online providers follow them.
ArtworkChain localArtworkChain();

// A disc subfolder name: "cd", "disc" or "disk" not preceded by a letter, or "碟", then optional spaces and a number.
bool isDiscFolderName(std::wstring_view name);
std::wstring albumTitleWithoutDiscSuffix(std::wstring_view title);

// Local images larger than this are not read.
constexpr uintmax_t kMaxArtworkBytes = 32ull << 20;

} // namespace camel::server
