// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The tests are built with the player's own compiler and linker settings (CamelPlayer.Cpp.props).
#include <gtest/gtest.h>

#include <windows.h>

#include <cstddef>

// The player parses media files and network answers: in a release build an indirect call may only reach the start of
// a function. The sanitizer builds, which look for those faults themselves, go without it.
TEST(BuildHardening, ReleaseBuildsHaveControlFlowGuard) {
#if defined(_DEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__clang__)
    GTEST_SKIP() << "only a release build has Control Flow Guard";
#else
    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    EXPECT_NE(nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF, 0);
    const auto& entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    ASSERT_GE(entry.Size, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, GuardFlags) + sizeof(DWORD));
    const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY*>(base + entry.VirtualAddress);
    EXPECT_NE(config->GuardFlags & IMAGE_GUARD_CF_INSTRUMENTED, 0u);
#endif
}
