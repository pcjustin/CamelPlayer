// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace ui {

// The minimize, maximize and close buttons of a window whose title bar is drawn by the app, in its theme's colors.
inline void colorCaptionButtons(winrt::Microsoft::UI::Windowing::AppWindow const& window, bool dark) {
    using winrt::Windows::UI::Color;
    const auto bar = window.TitleBar();
    const Color foreground = dark ? Color{255, 255, 255, 255} : Color{255, 0, 0, 0};
    const Color transparent{0, 0, 0, 0};
    bar.ButtonBackgroundColor(transparent);
    bar.ButtonInactiveBackgroundColor(transparent);
    bar.ButtonForegroundColor(foreground);
    bar.ButtonHoverForegroundColor(foreground);
    bar.ButtonPressedForegroundColor(foreground);
    bar.ButtonInactiveForegroundColor(dark ? Color{255, 140, 140, 140} : Color{255, 120, 120, 120});
    bar.ButtonHoverBackgroundColor(dark ? Color{24, 255, 255, 255} : Color{24, 0, 0, 0});
    bar.ButtonPressedBackgroundColor(dark ? Color{16, 255, 255, 255} : Color{16, 0, 0, 0});
}

} // namespace ui
