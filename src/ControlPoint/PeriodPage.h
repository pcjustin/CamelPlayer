// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ui {

constexpr int64_t kPeriodRankingLimit = 500; // the most the Server ranks of a period

template <class Row> struct PeriodPage {
    int64_t total = 0;
    std::vector<Row> rows;
};

// One page of a period's ranking for a list that reads it a page at a time: `rank` gives the ranking's first rows, up
// to the count it is given. The ranking is asked whole, up to its limit, as its size is known only from all its rows.
template <class Row>
PeriodPage<Row> periodPage(int64_t offset, int64_t limit, const std::function<std::vector<Row>(int64_t)>& rank) {
    const auto ranked = rank(kPeriodRankingLimit);
    PeriodPage<Row> page{int64_t(ranked.size()), {}};
    for (auto i = size_t(offset); i < ranked.size() && std::cmp_less(i, offset + limit); ++i) page.rows.push_back(ranked[i]);
    return page;
}

} // namespace ui
