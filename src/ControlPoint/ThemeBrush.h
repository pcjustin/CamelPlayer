// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Theme brushes for colors set from code. Application resources resolve for the Windows theme, so such a color would
// keep it while the app shows another theme; these come from the theme the app shows.
#pragma once

namespace ui {

// The brush `key` in `theme` (Default: the Application's), or in high contrast while Windows uses it.
winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(winrt::Microsoft::UI::Xaml::ElementTheme theme, const wchar_t* key);
// The brush `key` in the theme `element` shows.
winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                                                    const wchar_t* key);

} // namespace ui
