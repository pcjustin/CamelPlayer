// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "ControlPoint/Substitute.h"

// A value put in is not read for markers again: an album named "Demos {1}" stays so, and the real {1} gets its value.
TEST(Substitute, AValueHoldingAMarkerIsNotSubstitutedAgain) {
    EXPECT_EQ(ui::substitute(L"{0} by {1}", {L"Demos {1}", L"Ana"}), L"Demos {1} by Ana");
    EXPECT_EQ(ui::substitute(L"{1} after {0}", {L"first", L"second"}), L"second after first");
    EXPECT_EQ(ui::substitute(L"no markers", {L"x"}), L"no markers");
}
