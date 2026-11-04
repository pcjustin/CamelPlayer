// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <limits>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Documents.h>

#include "Server/Library/LibraryNames.h"
#include "UiText.h"

namespace ui {

// Shows an artist credit in `text` with each name the library lists as a link that passes the name to `open`.
inline void showCredit(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text,
                       const std::vector<camel::server::LibraryNamePart>& parts,
                       const std::function<void(winrt::hstring const&)>& open) {
    using namespace winrt::Microsoft::UI::Xaml::Documents;
    text.Inlines().Clear();
    for (const auto& part : parts) {
        Run run;
        run.Text(utf8(part.text));
        if (!part.name) {
            text.Inlines().Append(run);
            continue;
        }
        Hyperlink link;
        link.UnderlineStyle(UnderlineStyle::None);
        link.Inlines().Append(run);
        link.Click([open, name = utf8(part.text)](auto&&, auto&&) { open(name); });
        text.Inlines().Append(link);
    }
}

// Shows an artist credit in `panel` as link buttons with the text between them in `separator` style. Unlike a link
// inside text, a button keeps a list from also activating the row it sits in.
inline void showCreditButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel,
                              const std::vector<camel::server::LibraryNamePart>& parts,
                              const std::function<void(winrt::hstring const&)>& open,
                              winrt::Microsoft::UI::Xaml::Style const& separator, double fontSize) {
    using namespace winrt::Microsoft::UI::Xaml;
    panel.Children().Clear();
    for (const auto& part : parts) {
        if (!part.name) {
            Controls::TextBlock text;
            text.Style(separator);
            text.FontSize(fontSize);
            text.Text(utf8(part.text));
            panel.Children().Append(text);
            continue;
        }
        Controls::HyperlinkButton link;
        Controls::TextBlock name;
        name.Text(utf8(part.text));
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        link.Content(name);
        link.Padding(Thickness{});
        link.MinWidth(0);
        link.MinHeight(0);
        link.FontSize(fontSize);
        link.VerticalAlignment(VerticalAlignment::Center);
        link.Click([open, name = utf8(part.text)](auto&&, auto&&) { open(name); });
        panel.Children().Append(link);
    }
}

// Fits a credit shown by showCreditButtons in `width`, as text that does not fit ends in "…": the first name that does
// not fit ends in it, or "…" follows the names that do when there is no room to start the next; the panel's tooltip
// holds the whole credit.
inline void fitCreditButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, double width,
                             winrt::Microsoft::UI::Xaml::Style const& separator, double fontSize) {
    using namespace winrt::Microsoft::UI::Xaml;
    constexpr double kShortestName = 32; // room enough to start a name before its "…"
    // Widths that add up to the room exactly can sum a hair over it; such a name fits, or it would be "trimmed" to its
    // own width and the names after it would go without a "…".
    constexpr double kRounding = 0.5;
    const auto children = panel.Children();
    const auto ellipsis = winrt::hstring(L"…");
    if (const auto size = children.Size();
        size && winrt::unbox_value_or<winrt::hstring>(children.GetAt(size - 1).as<FrameworkElement>().Tag(), L"") == ellipsis)
        children.RemoveAtEnd();
    const winrt::Windows::Foundation::Size unlimited{std::numeric_limits<float>::infinity(),
                                                     std::numeric_limits<float>::infinity()};
    std::vector<double> widths;
    double total = 0;
    for (const auto& child : children) {
        const auto element = child.as<FrameworkElement>();
        element.Visibility(Visibility::Visible);
        element.MaxWidth(std::numeric_limits<double>::infinity());
        element.Measure(unlimited);
        widths.push_back(element.DesiredSize().Width);
        total += widths.back();
    }
    if (width <= 0 || total <= width + kRounding) return;
    double used = 0;
    size_t kept = 0;
    while (kept < widths.size() && used + widths[kept] <= width + kRounding) used += widths[kept++];
    if (const auto link = children.GetAt(uint32_t(kept)).try_as<Controls::HyperlinkButton>();
        link && width - used >= kShortestName) {
        link.MaxWidth(width - used);
        ++kept;
    } else {
        Controls::TextBlock more;
        more.Style(separator);
        more.FontSize(fontSize);
        more.Text(ellipsis);
        more.Tag(winrt::box_value(ellipsis));
        more.Measure(unlimited);
        while (kept && used + more.DesiredSize().Width > width) used -= widths[--kept];
        children.Append(more);
    }
    for (size_t i = kept; i < widths.size(); ++i) children.GetAt(uint32_t(i)).Visibility(Visibility::Collapsed);
}

} // namespace ui
