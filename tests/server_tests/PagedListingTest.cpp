// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include "Server/Api/PagedListing.h"

using camel::server::PagedListing;

namespace {

std::vector<int> page(int first, int count) {
    std::vector<int> out;
    out.reserve(size_t(count));
    for (int i = 0; i < count; ++i) out.push_back(first + i);
    return out;
}

TEST(PagedListing, RandomReadsQueueEachMissingPageOnce) {
    PagedListing<int> listing(10);
    EXPECT_EQ(listing.at(0), nullptr);
    EXPECT_EQ(listing.at(5), nullptr);
    EXPECT_EQ(listing.at(9999), nullptr); // before the first page the size is unknown, so any index may exist
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{0, 999}));
    EXPECT_EQ(listing.at(3), nullptr);
    EXPECT_TRUE(listing.takeWanted().empty()); // already in flight
    EXPECT_TRUE(listing.deliver(0, 7, 25, page(0, 10)));
    ASSERT_NE(listing.at(3), nullptr);
    EXPECT_EQ(*listing.at(3), 3);
    EXPECT_EQ(listing.size(), 25);
    EXPECT_EQ(listing.revision(), 7u);
    EXPECT_EQ(listing.at(25), nullptr); // past the end: not requested
    EXPECT_EQ(listing.at(24), nullptr);
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{2}));
    EXPECT_FALSE(listing.deliver(2, 7, 25, page(20, 5)));
    EXPECT_EQ(*listing.at(24), 24);
}

TEST(PagedListing, ANewerRevisionReplacesEveryPageAndAnOlderOneIsIgnored) {
    PagedListing<int> listing(10);
    listing.at(0);
    listing.at(10);
    listing.takeWanted();
    listing.deliver(0, 5, 20, page(0, 10));
    listing.deliver(1, 5, 20, page(10, 10));
    listing.at(0);
    listing.takeWanted();
    EXPECT_TRUE(listing.deliver(0, 6, 30, page(100, 10)));
    EXPECT_EQ(listing.size(), 30);
    EXPECT_EQ(*listing.at(0), 100);
    EXPECT_EQ(listing.at(10), nullptr); // page 1 belonged to revision 5
    listing.takeWanted();
    EXPECT_FALSE(listing.deliver(1, 5, 20, page(10, 10)));
    EXPECT_EQ(listing.at(10), nullptr);
    EXPECT_EQ(listing.revision(), 6u);
}

// A scan that changes the library while a page is fetched: the view reads every row again, and the page in flight
// under the old revision is asked for again under the new one, its old answer being dropped.
TEST(PagedListing, APageInFlightWhenTheRevisionChangesIsAskedForAgain) {
    PagedListing<int> listing(10);
    listing.at(0);
    listing.takeWanted();
    listing.deliver(0, 1, 30, page(0, 10));
    EXPECT_EQ(listing.at(15), nullptr);
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{1})); // in flight under revision 1
    listing.at(0);
    listing.takeWanted();
    ASSERT_TRUE(listing.deliver(0, 2, 30, page(100, 10))); // revision 2: the view reads every row again
    EXPECT_EQ(listing.at(15), nullptr);
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{1}));
    EXPECT_FALSE(listing.deliver(1, 1, 30, page(10, 10))); // the old answer
    EXPECT_EQ(listing.at(15), nullptr);
    EXPECT_TRUE(listing.takeWanted().empty()); // still in flight under revision 2
    EXPECT_FALSE(listing.deliver(1, 2, 30, page(110, 10)));
    EXPECT_EQ(*listing.at(15), 115);
}

TEST(PagedListing, MemoryStaysBoundedAndFailedPagesAreRetried) {
    PagedListing<int> listing(10, 3);
    for (int p = 0; p < 5; ++p) {
        listing.at(int64_t{p} * 10);
        listing.takeWanted();
        listing.deliver(p, 1, 50, page(p * 10, 10));
    }
    EXPECT_EQ(listing.at(0), nullptr); // the least recently read pages were dropped
    EXPECT_EQ(listing.at(15), nullptr);
    EXPECT_NE(listing.at(25), nullptr);
    EXPECT_NE(listing.at(45), nullptr);
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{0, 1}));
    listing.failed(0);
    listing.at(1);
    EXPECT_EQ(listing.takeWanted(), (std::vector<int64_t>{0}));
}

} // namespace
