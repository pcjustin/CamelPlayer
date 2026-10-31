// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The stand-in for an artist without a picture (docs/Plan.md §7.4): the name's first letter on a colour of its own.
#pragma once
#include <windows.h>

#include <array>
#include <cstdint>
#include <string_view>

#include <winrt/Windows.UI.h>

namespace ui {

// The first letter, digit or ideograph of `name`, in capitals; empty when it has none.
inline winrt::hstring initialOf(std::wstring_view name) {
    for (size_t i = 0; i < name.size(); ++i) {
        const size_t length = IS_HIGH_SURROGATE(name[i]) && i + 1 < name.size() ? 2 : 1;
        WORD type[2] = {};
        if (GetStringTypeW(CT_CTYPE1, name.data() + i, int(length), type) && (type[0] & (C1_ALPHA | C1_DIGIT))) {
            std::wstring letter(name.substr(i, length));
            CharUpperBuffW(letter.data(), DWORD(letter.size()));
            return winrt::hstring(letter);
        }
        i += length - 1;
    }
    return {};
}

// One of a few muted colours that white text reads on in both themes, the same for a name every time.
inline winrt::Windows::UI::Color avatarColor(std::wstring_view name) {
    static constexpr std::array<uint32_t, 8> kColors{0x6B5B95, 0x3F7A8C, 0x8C6A4A, 0x557A46,
                                                     0x8C4A5E, 0x4A5E8C, 0x7A6A2E, 0x5E5E6E};
    uint32_t hash = 2166136261u;
    for (const wchar_t c : name) hash = (hash ^ uint32_t(c)) * 16777619u;
    const auto rgb = kColors[hash % kColors.size()];
    return {255, uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)};
}

} // namespace ui
