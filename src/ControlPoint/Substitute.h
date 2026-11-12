// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <initializer_list>
#include <string>

namespace ui {

// `text` with {0}, {1}... replaced by `values`, in one pass, so that a value holding a marker is put in as it is.
inline std::wstring substitute(const std::wstring& text, std::initializer_list<std::wstring> values) {
    std::wstring out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find(L'{', at);
        if (open == std::wstring::npos) break;
        const size_t close = text.find(L'}', open);
        if (close == std::wstring::npos) break;
        out.append(text, at, open - at);
        const std::wstring digits = text.substr(open + 1, close - open - 1);
        size_t index = values.size();
        if (!digits.empty() && digits.size() < 4 && digits.find_first_not_of(L"0123456789") == std::wstring::npos)
            index = size_t(std::stoul(digits));
        if (index < values.size())
            out += values.begin()[index];
        else
            out.append(text, open, close - open + 1);
        at = close + 1;
    }
    out.append(text, at, std::wstring::npos);
    return out;
}

} // namespace ui
