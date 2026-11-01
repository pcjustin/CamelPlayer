// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "ThemeBrush.h"

#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace ui {

namespace {

ElementTheme shown = ElementTheme::Dark;

// `key` in the theme dictionary `theme` of `dictionary` or of a dictionary merged into it.
Windows::Foundation::IInspectable themed(ResourceDictionary const& dictionary, hstring const& theme,
                                         Windows::Foundation::IInspectable const& key) {
    if (const auto found = dictionary.ThemeDictionaries().TryLookup(box_value(theme)))
        if (const auto value = found.as<ResourceDictionary>().TryLookup(key)) return value;
    for (const auto& merged : dictionary.MergedDictionaries())
        if (const auto value = themed(merged, theme, key)) return value;
    return nullptr;
}

} // namespace

void setTheme(ElementTheme theme) {
    shown = theme;
}

Media::Brush themeBrush(const wchar_t* key) {
    static const Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    const auto resources = Application::Current().Resources();
    const auto name = box_value(key);
    // WinUI names the dark dictionary Default.
    const wchar_t* themes[2] = {L"Light", nullptr};
    if (accessibility.HighContrast())
        themes[0] = L"HighContrast";
    else if (shown == ElementTheme::Dark) {
        themes[0] = L"Dark";
        themes[1] = L"Default";
    }
    for (const wchar_t* theme : themes)
        if (theme)
            if (const auto value = themed(resources, theme, name)) return value.try_as<Media::Brush>();
    return resources.TryLookup(name).try_as<Media::Brush>();
}

} // namespace ui
