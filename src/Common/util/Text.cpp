// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/Text.h"

#include <windows.h>

namespace camel {

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring longPath(const std::filesystem::path& p) {
    std::wstring s = std::filesystem::absolute(p).lexically_normal().wstring();
    if (s.rfind(L"\\\\?\\", 0) == 0) return s;
    if (s.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + s.substr(2);
    return L"\\\\?\\" + s;
}

} // namespace camel
