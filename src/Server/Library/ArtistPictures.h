// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Artist pictures (docs/Plan.md §7.4): one per name, shared by an artist and a composer of that name, kept in the
// library database so they outlast rescans and rebuilds. The user's own picture wins over a found one.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Server/Artwork/OnlineArtistPictures.h"
#include "Server/Library/LibraryStore.h"

namespace camel::server {

struct ArtistPicture {
    bool chosen = false; // the user's own picture
    bool found = false;  // one found in the library or online, shown when there is no own picture
    std::string source;  // of the found picture: "folder", "embedded-artist" or "commons"
    // A Wikimedia Commons picture's author and license, and the file's page.
    std::string author, license, licenseUrl, page;
    std::string mbid;                        // the MusicBrainz artist the name was matched to
    std::vector<ArtistCandidate> candidates; // the MusicBrainz artists of that exact name, when there are several
    bool operator==(const ArtistPicture&) const = default;
};

// Pictures are kept at most this size, square.
constexpr unsigned kArtistPicturePixels = 512;

ArtistPicture artistPicture(const LibraryStore& store, const std::string& name);
// Looks the name up when due and keeps what it finds: an `artist.(jpg|jpeg|png|webp)` in a folder named exactly so, a
// lead artist picture in its tracks, then, with `online`, MusicBrainz, Wikidata and Wikimedia Commons. Nothing is
// looked up for a name with a picture, again after a found nothing only once 30 days have passed or when `online` was
// off then, after a failure at the next call a minute later, and when several artists share the name only online
// after 30 days; the library itself is searched again only after 30 days. A cancelled lookup keeps nothing.
ArtistPicture lookUpArtistPicture(LibraryStore& store, const std::string& name, bool online, const online::Services& services,
                                  const CancellationToken& token);
// The user's own picture from an image file's bytes; throws std::invalid_argument when they are not a picture.
ArtistPicture setArtistPicture(LibraryStore& store, const std::string& name, const std::vector<uint8_t>& image);
ArtistPicture removeArtistPicture(LibraryStore& store, const std::string& name);
// Takes the MusicBrainz artist `mbid` as the one the name means and looks that artist's picture up; the candidates
// stay, so another can be chosen. Throws std::runtime_error when the lookup fails.
ArtistPicture chooseArtistCandidate(LibraryStore& store, const std::string& name, const std::string& mbid,
                                    const online::Services& services, const CancellationToken& token);

} // namespace camel::server
