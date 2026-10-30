// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "CardSize.g.h"

namespace ui {

constexpr double kCover = 160;
constexpr double kMinCover = 140;

// The number of cards in a width whose covers come closest to kCover without going under kMinCover, each card taking
// `extra` beside its cover, and the cover edge that makes those cards fill the width.
struct CoverFit {
    size_t columns = 1;
    double cover = kCover;
};
CoverFit fitCovers(double width, double extra);

} // namespace ui

namespace winrt::CamelPlayer::implementation {

struct CardSize : CardSizeT<CardSize> {
    CardSize() = default;

    double Cover() const { return cover_; }
    void Cover(double cover);
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }

private:
    double cover_ = ui::kCover;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct CardSize : CardSizeT<CardSize, implementation::CardSize> {};

} // namespace winrt::CamelPlayer::factory_implementation
