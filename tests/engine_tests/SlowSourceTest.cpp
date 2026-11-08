// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A source read slower than it plays is buffered before it plays: how long, and which sources are measured.
#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/SlowSource.h"

using namespace camel::audio;

TEST(SlowSource, AFastEnoughSourceIsNotBuffered) {
    EXPECT_EQ(bufferSeconds(180.0, 1.1), 0.0);
    EXPECT_EQ(bufferSeconds(180.0, 3.0), 0.0);
    EXPECT_EQ(bufferSeconds(0.0, 0.5), 0.0);
}

TEST(SlowSource, TheBufferCoversTheRestOfTheFileAtTheReadRate) {
    // DSD256 from the owner's NAS: read at 2.2 MB/s, played at 2.82 MB/s, 163 s left.
    const double ratio = 2.2 / 2.82;
    const double wait = bufferSeconds(163.0, ratio);
    // Played from the moment the buffer holds `wait` seconds, while reading goes on at the rate measured less a margin,
    // the buffer never runs dry before the end.
    const double margin = 1.08;
    for (int halfSeconds = 0; halfSeconds <= 326; ++halfSeconds) {
        const double t = halfSeconds / 2.0;
        EXPECT_GE(wait + t * ratio / margin - t, 0.0) << t;
    }
    EXPECT_GT(wait, 163.0 * (1.0 - ratio));
    EXPECT_LT(wait, 163.0 * (1.0 - ratio) + 20.0);
}

TEST(SlowSource, ASlowerSourceWaitsLonger) {
    EXPECT_GT(bufferSeconds(163.0, 0.5), bufferSeconds(163.0, 0.8));
    EXPECT_GT(bufferSeconds(300.0, 0.8), bufferSeconds(163.0, 0.8));
    // Just fast enough still keeps a little in hand.
    EXPECT_GT(bufferSeconds(163.0, 1.05), 0.0);
}

TEST(SlowSource, OnlyNetworkPathsAreMeasured) {
    EXPECT_TRUE(onNetworkShare(L"\\\\tachikoma\\music\\a.dsf"));
    EXPECT_TRUE(onNetworkShare(L"\\\\?\\UNC\\tachikoma\\music\\a.dsf"));
    EXPECT_TRUE(onNetworkShare(L"\\\\?\\unc\\tachikoma\\music\\a.dsf"));
    wchar_t temp[MAX_PATH]{};
    ASSERT_TRUE(GetTempPathW(MAX_PATH, temp));
    EXPECT_FALSE(onNetworkShare(std::wstring(temp) + L"a.dsf"));
    EXPECT_FALSE(onNetworkShare(L"\\\\?\\" + std::wstring(temp) + L"a.dsf"));
    EXPECT_FALSE(onNetworkShare(L"a.dsf"));
}

TEST(SlowSource, TheReadRateComesFromTheFile) {
    const auto file = std::filesystem::temp_directory_path() / L"camel-slow-source.bin";
    {
        std::ofstream out(file, std::ios::binary);
        const std::vector<char> block(1 << 20, 'x');
        for (int i = 0; i < 3; ++i) out.write(block.data(), std::streamsize(block.size()));
    }
    const auto rate = measureReadRate(file.wstring(), 1 << 20, 2 << 20);
    ASSERT_TRUE(rate);
    EXPECT_GT(*rate, 0.0);
    EXPECT_FALSE(measureReadRate(file.wstring() + L".missing", 0, 1 << 20));
    std::filesystem::remove(file);
}
