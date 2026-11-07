// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Localized strings from Strings\<lang>\Resources.resw (docs/Plan.md §8.5).
#pragma once
#include <string>

#include "Common/AudioTypes.h"

namespace ui {

winrt::hstring tr(const wchar_t* key);
void reloadText(); // after the language changes
// The string `key` with {0}, {1}, … replaced by `values`.
winrt::hstring format(const wchar_t* key, std::initializer_list<std::wstring> values);
winrt::hstring errorTitle(camel::audio::ErrorCode code);
// A network player's reason for the error in full, or empty where errorTitle and the detail say it.
winrt::hstring playerErrorTitle(camel::audio::ErrorCode code);
winrt::hstring utf8(const std::string& s);
std::wstring formatTime(double seconds); // m:ss or h:mm:ss
std::wstring count(int64_t n);           // 70,715

} // namespace ui
