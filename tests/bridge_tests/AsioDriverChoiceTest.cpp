// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "OutputBridge/AsioDriverChoice.h"

using namespace camel::audio;

// With no driver saved, the first one that can be loaded: a registered driver whose device is gone is listed as
// unavailable and passed over.
TEST(AsioDriverChoice, WithNoneSavedTheFirstAvailableDriverIsLoaded) {
    const std::vector<DeviceEntry> drivers{{"{A}", "Gone (unavailable)"}, {"{B}", "Chord ASIO"}, {"{C}", "Other"}};
    EXPECT_EQ(asioDriverToLoad(drivers, ""), "{B}");
    EXPECT_EQ(asioDriverToLoad(drivers, "{C}"), "{C}") << "a saved driver is kept";
    EXPECT_THROW(asioDriverToLoad(drivers, "{A}"), Error) << "and never replaced";
    EXPECT_THROW(asioDriverToLoad({{"{A}", "Gone (unavailable)"}}, ""), Error) << "none available";
    EXPECT_THROW(asioDriverToLoad({}, ""), Error);
}
