// Localized strings from Strings\<lang>\Resources.resw (docs/Plan.md §8.5).
#pragma once
#include <string>

#include "audio/AudioTypes.h"

namespace ui {

winrt::hstring tr(const wchar_t* key);
winrt::hstring errorTitle(camel::audio::ErrorCode code);
winrt::hstring utf8(const std::string& s);
std::wstring formatTime(double seconds); // m:ss or h:mm:ss

} // namespace ui
