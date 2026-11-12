// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "UiText.h"

#include "Substitute.h"

namespace ui {

namespace {

winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader& loader() {
    static winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader current;
    return current;
}

} // namespace

void reloadText() {
    loader() = {};
}

winrt::hstring tr(const wchar_t* key) {
    try {
        auto s = loader().GetString(key);
        return s.empty() ? winrt::hstring(key) : s;
    } catch (...) {
        return winrt::hstring(key);
    }
}

winrt::hstring format(const wchar_t* key, std::initializer_list<std::wstring> values) {
    return winrt::hstring(substitute(std::wstring(tr(key)), values));
}

winrt::hstring errorTitle(camel::audio::ErrorCode code) {
    using camel::audio::ErrorCode;
    switch (code) {
    case ErrorCode::FileOpen:
        return tr(L"Error_FileOpen");
    case ErrorCode::UnsupportedStream:
        return tr(L"Error_UnsupportedStream");
    case ErrorCode::DecodeFailed:
        return tr(L"Error_DecodeFailed");
    case ErrorCode::SeekFailed:
        return tr(L"Error_SeekFailed");
    case ErrorCode::DeviceNotFound:
        return tr(L"Error_DeviceNotFound");
    case ErrorCode::DeviceInUse:
        return tr(L"Error_DeviceInUse");
    case ErrorCode::DeviceLost:
        return tr(L"Error_DeviceLost");
    case ErrorCode::FormatNotSupported:
        return tr(L"Error_FormatNotSupported");
    case ErrorCode::DriverError:
        return tr(L"Error_DriverError");
    case ErrorCode::Other:
        break;
    }
    return tr(L"Error_Other");
}

winrt::hstring playerErrorTitle(camel::audio::ErrorCode code) {
    using camel::audio::ErrorCode;
    switch (code) {
    case ErrorCode::DeviceInUse:
        return tr(L"Error_PlayerInUse");
    case ErrorCode::DeviceLost:
        return tr(L"Error_PlayerLost");
    case ErrorCode::DriverError:
        return tr(L"Error_PlayerDidNotPlay");
    default:
        return {};
    }
}

winrt::hstring utf8(const std::string& s) {
    return winrt::to_hstring(s);
}

std::wstring formatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto total = unsigned(seconds);
    wchar_t buf[32];
    if (total >= 3600)
        swprintf(buf, 32, L"%u:%02u:%02u", total / 3600, total / 60 % 60, total % 60);
    else
        swprintf(buf, 32, L"%u:%02u", total / 60, total % 60);
    return buf;
}

std::wstring count(int64_t n) {
    std::wstring text = std::to_wstring(n);
    for (auto at = int64_t(text.size()) - 3; at > 0; at -= 3) text.insert(size_t(at), L",");
    return text;
}

} // namespace ui
