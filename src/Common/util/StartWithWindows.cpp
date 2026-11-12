// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/StartWithWindows.h"

#include <windows.h>

#include <string>
#include <vector>

#include "Common/util/Log.h"

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

bool enable(const std::wstring& program, const Keys& keys) {
    const std::wstring command = commandFor(program);
    const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, keys.run.c_str(), keys.name.c_str(), REG_SZ, command.c_str(),
                                           DWORD((command.size() + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS) return true;
    log::warn("start with Windows: cannot add the Run entry (%ld)", static_cast<long>(status));
    return false;
}

bool disable(const Keys& keys) {
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, keys.run.c_str(), keys.name.c_str());
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return true;
    log::warn("start with Windows: cannot remove the Run entry (%ld)", static_cast<long>(status));
    return false;
}

void follow(const std::wstring& program, const Keys& keys) {
    const State s = state(keys);
    if (s.registered && s.command != commandFor(program)) enable(program, keys);
}

// The arguments as Windows splits them, a quoted one whole, so the word inside a file's path is not taken for one.
bool isBackgroundStart(std::wstring_view commandLine) {
    size_t at = 0;
    while (at < commandLine.size()) {
        while (at < commandLine.size() && (commandLine[at] == L' ' || commandLine[at] == L'\t')) ++at;
        std::wstring argument;
        bool quoted = false;
        for (; at < commandLine.size() && (quoted || (commandLine[at] != L' ' && commandLine[at] != L'\t')); ++at) {
            if (commandLine[at] == L'"')
                quoted = !quoted;
            else
                argument += commandLine[at];
        }
        if (argument == kBackgroundArgument) return true;
    }
    return false;
}

Launch launchFor(bool background, bool running) {
    if (running) return background ? Launch::Nothing : Launch::ShowRunning;
    return background ? Launch::StartHidden : Launch::Start;
}

} // namespace camel::startup
