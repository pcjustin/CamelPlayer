// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The names an artist or composer credit holds: "Aimer with chelly" credits Aimer and chelly.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace camel::server {
namespace sqlite {
class Database;
}

// A run of a credit as shown: a name, or the text between two names.
struct LibraryNamePart {
    std::string text;
    bool name = false;
    bool operator==(const LibraryNamePart&) const = default;
};

// What makes two spellings of a name the same: full- and half-width forms fold together, while case still tells names
// apart (LiSA is not Lisa).
std::string libraryNameKey(std::string_view name);

// Splits credits at the separators that always divide names (; ； 、 feat. ft. featuring with, and a bracket that opens
// with one of those words), then at & ＆ , ， / ／ × : ： x and vs where at least one part is a known name, except & and
// "and" before a name opening with "The" and any of them before an instrument, so "22/7", "Bob Marley & The Wailers"
// and "Yo-Yo Ma, cello" stay whole. Text in any other bracket belongs to the name.
class LibraryNameSplitter {
public:
    LibraryNameSplitter() = default;
    // A name is known when one of `credits` (every artist, composer and album artist) holds it alone once the separators
    // that always divide names are applied.
    explicit LibraryNameSplitter(const std::vector<std::string>& credits);
    // Adds the names `credit` holds alone, as a scan meets new tags.
    void learn(std::string_view credit);

    // The credit's names and the text between them; joined, the parts give back the credit.
    std::vector<LibraryNamePart> parts(std::string_view credit) const;
    std::vector<std::string> names(std::string_view credit) const;

private:
    std::unordered_set<std::string> known_;
};

// Changes whenever LibraryNameSplitter splits differently, so that libraries derive their names again.
constexpr std::string_view kLibraryNameRules = "3";

// A splitter that knows every artist and composer tag and album artist in the library.
LibraryNameSplitter loadLibraryNames(sqlite::Database& db);
// Rewrites the artist and composer names in track_values from the tags in track_tags, for `tracks` or every track when
// it is empty, and advances the library revision when any changed. Returns how many tracks' names changed.
size_t deriveLibraryNames(sqlite::Database& db, const LibraryNameSplitter& splitter, const std::vector<int64_t>& tracks = {});
// Derives every track's names again unless they already follow kLibraryNameRules.
void updateLibraryNameRules(sqlite::Database& db);

} // namespace camel::server
