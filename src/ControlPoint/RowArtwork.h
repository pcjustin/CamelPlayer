// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <string_view>

namespace ui {

// Whether a change a row's item raised, by the name of the property, reads the row's artwork again: only a placeholder
// filled with its row, which raises every property at once (""), changes what the artwork shows.
inline bool changesRowArtwork(std::wstring_view property) {
    return property.empty();
}

} // namespace ui
