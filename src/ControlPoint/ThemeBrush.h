// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Theme brushes for colors set from code. Application resources resolve for the Windows theme, so such a color would
// keep it while the app shows another theme; these come from the theme the app shows.
#pragma once

#include <functional>

namespace ui {

// The brush `key` in `theme` (Default: the Application's), or in high contrast while Windows uses it.
winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(winrt::Microsoft::UI::Xaml::ElementTheme theme, const wchar_t* key);
// The brush `key` in the theme `element` shows.
winrt::Microsoft::UI::Xaml::Media::Brush themeBrush(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                                                    const wchar_t* key);

// The brush `key` for a line or shape drawn from code. A contrast theme gives WinUI's fill brushes the window's own
// colour, as they paint backgrounds there, so a shape filled with one would vanish: it takes the highlight colour for an
// accent, the disabled text colour for a disabled fill and the text colour for any other.
winrt::Microsoft::UI::Xaml::Media::Brush shapeBrush(winrt::Microsoft::UI::Xaml::ElementTheme theme, const wchar_t* key);
winrt::Microsoft::UI::Xaml::Media::Brush shapeBrush(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                                                    const wchar_t* key);

// Calls `paint` on `element`'s thread when its theme changes, and when a contrast theme is turned on or off or another
// is chosen, which WinUI does not count as a change of theme while the window stays light: what `element` paints from
// code follows both.
void followTheme(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, std::function<void()> paint);

// The black theme: the dark theme's surface brushes recoloured pure black, and back.
void showBlackSurfaces(bool black);
bool blackSurfacesShown();

} // namespace ui
