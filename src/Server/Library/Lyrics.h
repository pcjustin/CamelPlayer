// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A track's lyrics (docs/Plan.md §8.1): embedded in its tags or in a .lrc beside it, read when it plays; nothing is
// fetched online.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include "Server/Api/ServerApi.h"

namespace camel::server {

// LRC: lines led by one or more [mm:ss.xx] stamps are synced and sorted by time, lines of one stamp kept in the order
// written; [offset:] moves them; ID tags such as [by:] are left out. Text with no stamp at all is unsynced lyrics.
Lyrics parseLrc(std::string_view text);

// The track's lyrics: synced over unsynced, and a .lrc of the same name over embedded ones of the same kind. Empty when
// there are none or the file cannot be read.
Lyrics readLyrics(const std::filesystem::path& file);

// The first line of the stamp being sung at `ms`; nothing before the first.
std::optional<size_t> lyricLineAt(const Lyrics& lyrics, uint32_t ms);

} // namespace camel::server
