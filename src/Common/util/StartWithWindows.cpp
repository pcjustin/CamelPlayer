// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/StartWithWindows.h"

#include <windows.h>

#include <vector>

namespace camel::startup {

namespace {

std::wstring readString(const std::wstring& key, const std::wstring& name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

// Windows keeps 12 bytes for each entry it lists; an odd first byte is an entry turned off.
bool disabled(const Keys& keys) {
    std::vector<BYTE> data(12);
    auto bytes = DWORD(data.size());
    if (RegGetValueW(HKEY_CURRENT_USER, keys.approved.c_str(), keys.name.c_str(), RRF_RT_REG_BINARY, nullptr, data.data(),
                     &bytes) != ERROR_SUCCESS ||
        bytes == 0)
        return false;
    return (data[0] & 1) != 0;
}

} // namespace

std::wstring commandFor(const std::wstring& program) {
    return L"\"" + program + L"\" " + kBackgroundArgument;
}

State state(const Keys& keys) {
    State s;
    s.command = readString(keys.run, keys.name);
    s.registered = !s.command.empty();
    s.disabledInWindows = s.registered && disabled(keys);
    return s;
}

void enable(const std::wstring& program, const Keys& keys) {
    const std::wstring command = commandFor(program);
    RegSetKeyValueW(HKEY_CURRENT_USER, keys.run.c_str(), keys.name.c_str(), REG_SZ, command.c_str(),
                    DWORD((command.size() + 1) * sizeof(wchar_t)));
}

void disable(const Keys& keys) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, keys.run.c_str(), keys.name.c_str());
}

void follow(const std::wstring& program, const Keys& keys) {
    const State s = state(keys);
    if (s.registered && s.command != commandFor(program)) enable(program, keys);
}

bool isBackgroundStart(std::wstring_view commandLine) {
    for (size_t at = commandLine.find(kBackgroundArgument); at != std::wstring_view::npos;
         at = commandLine.find(kBackgroundArgument, at + 1)) {
        const size_t end = at + std::wstring_view(kBackgroundArgument).size();
        const bool startsWord = at == 0 || commandLine[at - 1] == L' ' || commandLine[at - 1] == L'"';
        const bool endsWord = end == commandLine.size() || commandLine[end] == L' ' || commandLine[end] == L'"';
        if (startsWord && endsWord) return true;
    }
    return false;
}

Launch launchFor(bool background, bool running) {
    if (running) return background ? Launch::Nothing : Launch::ShowRunning;
    return background ? Launch::StartHidden : Launch::Start;
}

} // namespace camel::startup
