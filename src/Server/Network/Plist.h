// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Apple's binary property lists (bplist00), as AirPlay 2 sends them in SETUP and its answers (docs/Plan.md §5.10): only
// the kinds those carry. What a receiver sends is read with its size and depth limited.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace camel::server::airplay {

struct Plist {
    enum class Kind : uint8_t { Boolean, Integer, Real, String, Data, Array, Dict };

    Kind kind = Kind::Dict;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    std::string text;              // a String in UTF-8, or Data's bytes
    std::vector<std::string> keys; // a Dict's, in order
    std::vector<Plist> items;      // an Array's, or a Dict's values beside `keys`

    static Plist of(bool value);
    static Plist of(int64_t value);
    static Plist of(int value) { return of(int64_t{value}); }
    static Plist of(double value);
    static Plist of(std::string_view text);
    static Plist of(const char* text) { return of(std::string_view(text)); }
    static Plist data(std::string_view bytes);
    static Plist array(std::vector<Plist> items);
    static Plist dict();

    Plist& set(std::string key, Plist value); // adds or replaces, keeping the order keys were added in
    const Plist* find(std::string_view key) const;
    bool operator==(const Plist&) const = default;
};

std::string toBinaryPlist(const Plist& root);
std::optional<Plist> parseBinaryPlist(std::string_view bytes);

} // namespace camel::server::airplay
