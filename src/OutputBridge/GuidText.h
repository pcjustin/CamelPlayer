// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A GUID as "{...}" in upper case, the form container IDs are compared in (docs/Plan.md §5.12).
#pragma once
#include <windows.h>

#include <cstdio>
#include <string>

namespace camel::audio {

inline std::string guidText(const GUID& g) {
    char text[40];
    const int length =
        snprintf(text, sizeof text, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned long>(g.Data1),
                 unsigned(g.Data2), unsigned(g.Data3), unsigned(g.Data4[0]), unsigned(g.Data4[1]), unsigned(g.Data4[2]),
                 unsigned(g.Data4[3]), unsigned(g.Data4[4]), unsigned(g.Data4[5]), unsigned(g.Data4[6]), unsigned(g.Data4[7]));
    return length == 38 ? std::string(text, size_t(length)) : std::string();
}

} // namespace camel::audio
