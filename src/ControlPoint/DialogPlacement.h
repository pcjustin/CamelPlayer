// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Dialogs open in a layer of their own that does not inherit the theme the window content requests.
#pragma once

namespace ui {

// Opens `dialog` over `root` in that window's light or dark theme.
inline void placeDialog(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog,
                        winrt::Microsoft::UI::Xaml::XamlRoot const& root) {
    dialog.XamlRoot(root);
    if (const auto content = root.Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        dialog.RequestedTheme(content.ActualTheme());
}

} // namespace ui
