// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "ControlPoint/AlbumBadges.h"

// A page opened after an album's keeps none of its badges, which a change of theme would otherwise draw on it.
TEST(AlbumBadges, AnotherPageKeepsNoneOfTheAlbumsBadges) {
    ui::AlbumBadges badges;
    camel::server::LibraryAlbumFormat flac;
    flac.tracks = 10;
    badges.set({flac});
    ASSERT_EQ(badges.formats().size(), 1U);
    badges.open();
    EXPECT_TRUE(badges.formats().empty());
}
