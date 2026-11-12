// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "ControlPoint/RowArtwork.h"

// A row's artwork is read again only when its placeholder is filled, which raises every property at once (""): a heart,
// the playing mark or a folder opening changes the row but not its cover.
TEST(RowArtwork, OnlyAFilledRowReadsItsArtworkAgain) {
    EXPECT_TRUE(ui::changesRowArtwork(L""));
    for (const auto* property :
         {L"IsFavorite", L"FavoriteGlyph", L"ShowsNumber", L"IsPlaying", L"IsNotPlaying", L"PlayingGlyph", L"ChevronGlyph"})
        EXPECT_FALSE(ui::changesRowArtwork(property)) << std::wstring(property).size();
}
