// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>

namespace ui {

// A text that ends in "…" shows its whole text as a tooltip, kept to its text while it stays cut short; a text that fits
// has none. Called from IsTextTrimmedChanged.
inline void tipWhenTrimmed(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text) {
    using namespace winrt::Microsoft::UI::Xaml;
    if (!text.IsTextTrimmed()) {
        Controls::ToolTipService::SetToolTip(text, nullptr);
        return;
    }
    Data::Binding binding;
    binding.Source(text);
    binding.Path(PropertyPath(L"Text"));
    Controls::ToolTip tip;
    tip.SetBinding(Controls::ContentControl::ContentProperty(), binding);
    Controls::ToolTipService::SetToolTip(text, tip);
}

} // namespace ui
