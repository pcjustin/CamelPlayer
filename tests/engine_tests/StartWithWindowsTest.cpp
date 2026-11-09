// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Starting with Windows, on keys of the tests' own so the user's Run key is never touched.
#include <gtest/gtest.h>

#include <windows.h>

#include <string>

#include "Common/util/StartWithWindows.h"

using namespace camel::startup;

namespace {

class StartWithWindowsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::wstring root = L"Software\\CamelPlayerTests\\StartWithWindows" + std::to_wstring(GetCurrentProcessId());
        keys_.run = root + L"\\Run";
        keys_.approved = root + L"\\StartupApproved";
    }
    void TearDown() override {
        RegDeleteTreeW(HKEY_CURRENT_USER,
                       (L"Software\\CamelPlayerTests\\StartWithWindows" + std::to_wstring(GetCurrentProcessId())).c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\CamelPlayerTests"); // only once no other test process holds a key in it
    }

    void approve(BYTE first) const {
        const BYTE data[12] = {first};
        RegSetKeyValueW(HKEY_CURRENT_USER, keys_.approved.c_str(), keys_.name.c_str(), REG_BINARY, data, sizeof(data));
    }

    Keys keys_;
};

} // namespace

TEST_F(StartWithWindowsTest, TheEntryIsWrittenWithTheBackgroundArgumentAndRemoved) {
    EXPECT_FALSE(state(keys_).registered);
    enable(L"C:\\Apps\\CamelPlayer\\CamelPlayer.exe", keys_);
    const State on = state(keys_);
    EXPECT_TRUE(on.on());
    EXPECT_EQ(on.command, L"\"C:\\Apps\\CamelPlayer\\CamelPlayer.exe\" --background");
    disable(keys_);
    EXPECT_FALSE(state(keys_).registered);
}

TEST_F(StartWithWindowsTest, AnEntryForAMovedFolderFollowsTheProgramRunning) {
    enable(L"C:\\Old\\CamelPlayer.exe", keys_);
    follow(L"D:\\New\\CamelPlayer.exe", keys_);
    EXPECT_EQ(state(keys_).command, commandFor(L"D:\\New\\CamelPlayer.exe"));
}

TEST_F(StartWithWindowsTest, NoEntryIsLeftAlone) {
    follow(L"D:\\New\\CamelPlayer.exe", keys_);
    EXPECT_FALSE(state(keys_).registered);
}

TEST_F(StartWithWindowsTest, WindowsTurningTheEntryOffIsReadAndKept) {
    enable(L"C:\\Apps\\CamelPlayer.exe", keys_);
    approve(3);
    EXPECT_TRUE(state(keys_).registered);
    EXPECT_TRUE(state(keys_).disabledInWindows);
    EXPECT_FALSE(state(keys_).on());
    follow(L"D:\\New\\CamelPlayer.exe", keys_);
    EXPECT_TRUE(state(keys_).disabledInWindows);
    approve(2);
    EXPECT_TRUE(state(keys_).on());
}

TEST(StartWithWindowsLaunchTest, TheBackgroundArgumentStartsHidden) {
    EXPECT_TRUE(isBackgroundStart(L"--background"));
    EXPECT_TRUE(isBackgroundStart(L"\"C:\\Apps\\CamelPlayer.exe\" --background"));
    EXPECT_FALSE(isBackgroundStart(L""));
    EXPECT_FALSE(isBackgroundStart(L"--after-reset"));
    EXPECT_FALSE(isBackgroundStart(L"--backgrounds"));
    EXPECT_EQ(launchFor(true, false), Launch::StartHidden);
    EXPECT_EQ(launchFor(false, false), Launch::Start);
}

TEST(StartWithWindowsLaunchTest, ABackgroundStartLeavesARunningInstanceAlone) {
    EXPECT_EQ(launchFor(true, true), Launch::Nothing);
    EXPECT_EQ(launchFor(false, true), Launch::ShowRunning);
}
