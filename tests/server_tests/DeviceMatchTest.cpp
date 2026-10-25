// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "Server/Playback/DeviceMatch.h"

using namespace camel;
using namespace camel::server;

namespace {

// Names as Windows reported them for a Chord DAVE and a built-in Cirrus codec (2026-10-25).
const DeviceInfo kWasapiCirrus{"w-cirrus", "喇叭 (Cirrus Logic CS4208 (AB 126))"};
const DeviceInfo kWasapiDave{"w-dave", "數位輸出 (Chord Async USB 44.1kHz-768kHz)"};
const DeviceInfo kAsioDave{"a-dave", "ASIO Chord  1.05"};
const DeviceInfo kKsDave{"k-dave", "Chord Async USB 44.1kHz-768kHz / Chord Electronics Ltd. Streaming [Standard]"};
const DeviceInfo kKsCirrus{"k-cirrus", "Cirrus Logic CS4208 (AB 126) / HD Audio 喇叭 · 電腦喇叭 [WaveRT]"};

const std::vector<DeviceInfo> kWasapi{kWasapiCirrus, kWasapiDave};
const std::vector<DeviceInfo> kAsio{kAsioDave};
const std::vector<DeviceInfo> kKs{kKsDave, kKsCirrus};

} // namespace

TEST(DeviceMatch, FollowsTheSameDacAcrossEveryBackend) {
    EXPECT_EQ(matchDevice(Backend::Wasapi, kWasapiDave, Backend::Ks, kKs), "k-dave");
    EXPECT_EQ(matchDevice(Backend::Wasapi, kWasapiDave, Backend::Asio, kAsio), "a-dave");
    EXPECT_EQ(matchDevice(Backend::Asio, kAsioDave, Backend::Wasapi, kWasapi), "w-dave");
    EXPECT_EQ(matchDevice(Backend::Asio, kAsioDave, Backend::Ks, kKs), "k-dave");
    EXPECT_EQ(matchDevice(Backend::Ks, kKsDave, Backend::Wasapi, kWasapi), "w-dave");
    EXPECT_EQ(matchDevice(Backend::Ks, kKsCirrus, Backend::Wasapi, kWasapi), "w-cirrus");
}

TEST(DeviceMatch, NeverGuessesAnotherDevice) {
    EXPECT_FALSE(matchDevice(Backend::Wasapi, kWasapiCirrus, Backend::Asio, kAsio));
    EXPECT_FALSE(matchDevice(Backend::Wasapi, {"w-missing", ""}, Backend::Ks, kKs));
    EXPECT_FALSE(matchDevice(Backend::Wasapi, kWasapiDave, Backend::Ks, {}));
    const DeviceInfo secondPin{"k-dave-2", "Chord Async USB 44.1kHz-768kHz / Chord Electronics Ltd. S/PDIF [Standard]"};
    EXPECT_FALSE(matchDevice(Backend::Wasapi, kWasapiDave, Backend::Ks, {kKsDave, secondPin}));
    EXPECT_FALSE(matchDevice(Backend::Asio, kAsioDave, Backend::Ks, {kKsDave, secondPin}));
}
