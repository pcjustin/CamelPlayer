// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Artist pictures from the internet (docs/Plan.md §7.4): MusicBrainz names the artist, Wikidata its picture (P18) and
// Wikimedia Commons serves it, freely licensed, with its author and license.
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Server/Artwork/ArtworkProvider.h"

namespace camel::server {

struct ArtistCandidate {
    std::string mbid, name;
    std::string description; // MusicBrainz's disambiguation, or the artist's type, area and year of birth
    bool operator==(const ArtistCandidate&) const = default;
};

namespace online {

// Where the lookups go; tests point them at a local server.
struct Services {
    std::string musicbrainz = "https://musicbrainz.org";
    std::string wikidata = "https://www.wikidata.org";
    std::string commons = "https://commons.wikimedia.org";
    std::chrono::milliseconds musicbrainzInterval{1000}; // MusicBrainz allows one request a second
    std::chrono::milliseconds backoff{2000};             // the first wait after HTTP 429 or 503; it doubles once
    bool allowHttp = false;                              // picture links must be https unless set
};

struct CommonsImage {
    std::string url;  // a 960-pixel rendering, or the file itself when smaller
    std::string page; // the file's description page
    std::string author, license, licenseUrl;
};

struct Found {
    std::vector<ArtistCandidate> candidates; // every MusicBrainz artist named exactly so; a picture needs just one
    std::string mbid;                        // the artist the picture belongs to
    std::optional<CommonsImage> image;
    std::vector<uint8_t> bytes; // the downloaded picture
};

// Searches MusicBrainz for `name` (an artist's name or alias, matched exactly) and, when one artist has it, fetches
// that artist's picture. Throws std::runtime_error on network failures, other HTTP errors and cancellation.
Found findArtistPicture(const std::string& name, const Services& services, const CancellationToken& token);
// The picture of a known MusicBrainz artist.
Found pictureOf(const std::string& mbid, const Services& services, const CancellationToken& token);

// The steps' readers, for tests. Malformed JSON throws.
std::vector<ArtistCandidate> matchingArtists(const std::string& searchJson, const std::string& name);
std::optional<std::string> wikidataItem(const std::string& artistJson);
std::optional<std::string> pictureFile(const std::string& claimsJson);
std::optional<CommonsImage> commonsImage(const std::string& queryJson);
// Commons gives authors as HTML; this keeps their text.
std::string plainText(std::string_view html);

} // namespace online
} // namespace camel::server
