// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "ThemeBrush.h"

#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace ui {

namespace {

using Windows::UI::ViewManagement::UISettings;

bool highContrast() {
    static const Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    return accessibility.HighContrast();
}

// One instance for the system colours and their changes: an instance raises ColorValuesChanged only while it lives.
UISettings& systemColors() {
    static UISettings settings;
    return settings;
}

// `key` in the theme dictionary `theme` of `dictionary` or of a dictionary merged into it.
Windows::Foundation::IInspectable themed(ResourceDictionary const& dictionary, hstring const& theme,
                                         Windows::Foundation::IInspectable const& key) {
    if (const auto found = dictionary.ThemeDictionaries().TryLookup(box_value(theme)))
        if (const auto value = found.as<ResourceDictionary>().TryLookup(key)) return value;
    for (const auto& merged : dictionary.MergedDictionaries())
        if (const auto value = themed(merged, theme, key)) return value;
    return nullptr;
}

// The dark theme's surface brushes. WinUI's dictionary keeps one brush per key, which its controls' own keys alias, so
// recolouring these turns every surface that uses them black at once.
constexpr const wchar_t* kSurfaces[] = {
    L"ApplicationPageBackgroundThemeBrush",
    L"SolidBackgroundFillColorBaseBrush",
    L"SolidBackgroundFillColorBaseAltBrush",
    L"SolidBackgroundFillColorSecondaryBrush",
    L"SolidBackgroundFillColorTertiaryBrush",
    L"SolidBackgroundFillColorQuarternaryBrush",
    L"LayerFillColorDefaultBrush",
    L"CardBackgroundFillColorDefaultBrush",
    L"CardBackgroundFillColorSecondaryBrush",
};

// What stands above the page: menus, flyouts, drop-downs, tooltips and a dialog's content. On a black surface of their
// own they would merge into the page, their shadows unseen; a near-black one sets them apart.
constexpr const wchar_t* kRaised[] = {
    L"LayerFillColorAltBrush", // a dialog's content, above its buttons' base
    L"AcrylicBackgroundFillColorDefaultBrush",
    L"AcrylicBackgroundFillColorBaseBrush",
    L"AcrylicInAppFillColorDefaultBrush",
    L"AcrylicInAppFillColorBaseBrush",
    L"DesktopAcrylicTransparentBrush",
};

// The dark theme's strokes that darken what is under them, which black hides: they lighten it instead.
constexpr const wchar_t* kStrokes[] = {
    L"CardStrokeColorDefaultBrush",
    L"SurfaceStrokeColorFlyoutBrush",
};

// A brush as the dark theme had it: a solid brush's colour, an acrylic one's fallback.
struct Kept {
    Media::Brush brush;
    Windows::UI::Color color;
    bool alwaysFallback = false;
};

struct BlackSurfaces {
    bool shown = false;
    std::vector<Kept> kept;
};

BlackSurfaces& blackSurfaces() {
    static BlackSurfaces surfaces;
    return surfaces;
}

} // namespace

void showBlackSurfaces(bool black) {
    auto& surfaces = blackSurfaces();
    if (black == surfaces.shown) return;
    surfaces.shown = black;
    const Windows::UI::Color pureBlack = Microsoft::UI::Colors::Black();
    if (!black) {
        for (const auto& kept : surfaces.kept) {
            if (const auto solid = kept.brush.try_as<Media::SolidColorBrush>()) solid.Color(kept.color);
            if (const auto acrylic = kept.brush.try_as<Media::AcrylicBrush>()) {
                acrylic.FallbackColor(kept.color);
                acrylic.AlwaysUseFallback(kept.alwaysFallback);
            }
        }
        surfaces.kept.clear();
        return;
    }
    const auto resources = Application::Current().Resources();
    const auto recolour = [&](const wchar_t* key, Windows::UI::Color color) {
        const auto name = box_value(key);
        auto found = themed(resources, L"Dark", name);
        if (!found) found = themed(resources, L"Default", name);
        const auto brush = found.try_as<Media::Brush>();
        if (!brush || std::ranges::any_of(surfaces.kept, [&](const Kept& kept) { return kept.brush == brush; })) return;
        if (const auto solid = brush.try_as<Media::SolidColorBrush>()) {
            surfaces.kept.push_back({brush, solid.Color()});
            solid.Color(color);
        } else if (const auto acrylic = brush.try_as<Media::AcrylicBrush>()) {
            surfaces.kept.push_back({brush, acrylic.FallbackColor(), acrylic.AlwaysUseFallback()});
            acrylic.FallbackColor(color);
            acrylic.AlwaysUseFallback(true);
        }
    };
    for (const wchar_t* key : kSurfaces) recolour(key, pureBlack);
    for (const wchar_t* key : kRaised) recolour(key, Windows::UI::Color{0xFF, 0x1C, 0x1C, 0x1C});
    for (const wchar_t* key : kStrokes) recolour(key, Windows::UI::Color{0x29, 0xFF, 0xFF, 0xFF});
}

bool blackSurfacesShown() {
    return blackSurfaces().shown;
}

Media::Brush themeBrush(ElementTheme theme, const wchar_t* key) {
    const auto resources = Application::Current().Resources();
    const auto name = box_value(key);
    // WinUI names the dark dictionary Default.
    const wchar_t* themes[2] = {L"Light", nullptr};
    if (highContrast())
        themes[0] = L"HighContrast";
    else if (theme == ElementTheme::Dark ||
             (theme == ElementTheme::Default && Application::Current().RequestedTheme() == ApplicationTheme::Dark)) {
        themes[0] = L"Dark";
        themes[1] = L"Default";
    }
    for (const wchar_t* dictionary : themes)
        if (dictionary)
            if (const auto value = themed(resources, dictionary, name)) return value.try_as<Media::Brush>();
    return resources.TryLookup(name).try_as<Media::Brush>();
}

Media::Brush themeBrush(FrameworkElement const& element, const wchar_t* key) {
    return themeBrush(element.ActualTheme(), key);
}

Media::Brush shapeBrush(ElementTheme theme, const wchar_t* key) {
    if (!highContrast()) return themeBrush(theme, key);
    using Windows::UI::ViewManagement::UIElementType;
    const std::wstring_view name(key);
    const auto type = name.starts_with(L"Accent")                         ? UIElementType::Highlight
                      : name.find(L"Disabled") != std::wstring_view::npos ? UIElementType::GrayText
                                                                          : UIElementType::WindowText;
    return Media::SolidColorBrush(systemColors().UIElementColor(type));
}

Media::Brush shapeBrush(FrameworkElement const& element, const wchar_t* key) {
    return shapeBrush(element.ActualTheme(), key);
}

void followTheme(FrameworkElement const& element, std::function<void()> paint) {
    // The colours' subscription lives as long as the element's own handler, which holds it.
    auto changed = std::make_shared<UISettings::ColorValuesChanged_revoker>(systemColors().ColorValuesChanged(
        auto_revoke, [queue = element.DispatcherQueue(), weak = make_weak(element), paint](auto&&, auto&&) {
            queue.TryEnqueue([weak, paint] {
                if (weak.get()) paint();
            });
        }));
    element.ActualThemeChanged([changed, paint = std::move(paint)](auto&&, auto&&) { paint(); });
}

} // namespace ui
