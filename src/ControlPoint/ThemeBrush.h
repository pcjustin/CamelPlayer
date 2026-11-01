// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Theme brushes for colors set from code. Application resources resolve for the Windows theme, so such a color would
// keep it while the app shows another theme; these come from the theme the app shows.
#pragma once

namespace ui {

// The theme the window shows, Light or Dark, kept whenever it changes.
void setTheme(winrt::Microsoft::UI::Xaml::ElementTheme theme);
// The brush `key` in that theme, or in high contrast while Windows uses it.
winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(const wchar_t* key);

} // namespace ui
