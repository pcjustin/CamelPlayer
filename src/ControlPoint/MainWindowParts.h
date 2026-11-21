// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the main window's source files share, defined in MainWindow.xaml.cpp.
#pragma once

#include <vector>

#include "Common/AudioTypes.h"

namespace winrt::CamelPlayer::implementation::mainwindow {

const wchar_t* dspNodeKey(camel::audio::DspNode node);
Microsoft::UI::Xaml::Controls::ContentDialog textDialog(Microsoft::UI::Xaml::XamlRoot const& root, hstring const& title,
                                                        hstring const& text);
void fill(Microsoft::UI::Xaml::Controls::ComboBox const& combo, std::vector<hstring> const& items, int selected);
void setAnnounced(Microsoft::UI::Xaml::Controls::TextBlock const& block, hstring const& value);
void setBadgeTip(Microsoft::UI::Xaml::FrameworkElement const& badge, hstring const& name, hstring const& tip);
hstring decibels(double db);

} // namespace winrt::CamelPlayer::implementation::mainwindow
