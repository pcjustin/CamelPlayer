// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <utility>
#include <vector>

#include "Server/Api/ServerApi.h"

namespace ui {

// The format badges of the album a library page shows, kept so that a change of theme draws them again.
class AlbumBadges {
public:
    // Another page opens, with no album's badges until it sets its own.
    void open() { formats_.clear(); }
    void set(std::vector<camel::server::LibraryAlbumFormat> formats) { formats_ = std::move(formats); }
    const std::vector<camel::server::LibraryAlbumFormat>& formats() const { return formats_; }

private:
    std::vector<camel::server::LibraryAlbumFormat> formats_;
};

} // namespace ui
