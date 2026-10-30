// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "CardSize.h"
#if __has_include("CardSize.g.cpp")
#include "CardSize.g.cpp"
#endif

#include <algorithm>
#include <cmath>

namespace ui {

CoverFit fitCovers(double width, double extra) {
    // A pixel of slack keeps layout rounding from pushing the last card onto the next row.
    const double room = width - 1;
    const double nearest = std::round(room / (kCover + extra));
    const double most = std::floor(room / (kMinCover + extra));
    CoverFit fit;
    fit.columns = size_t(std::max(1.0, std::min(nearest, most)));
    fit.cover = std::max(kMinCover, std::floor(room / double(fit.columns)) - extra);
    return fit;
}

} // namespace ui

namespace winrt::CamelPlayer::implementation {

void CardSize::Cover(double cover) {
    if (cover == cover_) return;
    cover_ = cover;
    changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(L"Cover"));
}

} // namespace winrt::CamelPlayer::implementation
