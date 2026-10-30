// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
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
        link.Content(winrt::box_value(utf8(part.text)));
        link.Padding(Thickness{});
        link.MinWidth(0);
        link.MinHeight(0);
        link.FontSize(fontSize);
        link.VerticalAlignment(VerticalAlignment::Center);
        link.Click([open, name = utf8(part.text)](auto&&, auto&&) { open(name); });
        panel.Children().Append(link);
    }
}

} // namespace ui
