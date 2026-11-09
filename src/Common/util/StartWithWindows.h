// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Starting with Windows (Settings › Interface): CamelPlayer's own entry under the user's Run key, which Windows starts
// at sign-in with the background argument. Windows' startup settings and Task Manager may disable the entry; CamelPlayer
// reads that and never turns it back on.
#pragma once

#include <string>
#include <string_view>

namespace camel::startup {

inline constexpr wchar_t kBackgroundArgument[] = L"--background";

// Where the entry lives, apart for tests.
struct Keys {
    std::wstring run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    std::wstring approved = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    std::wstring name = L"CamelPlayer";
};

struct State {
    bool registered = false;        // the entry exists
    bool disabledInWindows = false; // Windows' startup settings turned it off
    std::wstring command;           // what it starts
    bool on() const { return registered && !disabledInWindows; }
};

std::wstring commandFor(const std::wstring& program); // the program quoted, with the background argument
State state(const Keys& keys = {});
void enable(const std::wstring& program, const Keys& keys = {});
void disable(const Keys& keys = {});
// An entry that starts another program, as after the folder moved, is pointed at this one; none is left alone.
void follow(const std::wstring& program, const Keys& keys = {});

bool isBackgroundStart(std::wstring_view commandLine);

// What a start does: a background start with an instance running leaves it alone, any other shows the running window.
enum class Launch { Start, StartHidden, ShowRunning, Nothing };
Launch launchFor(bool background, bool running);

} // namespace camel::startup
