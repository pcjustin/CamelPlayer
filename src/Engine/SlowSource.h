// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A source read slower than it plays, such as DSD256 from a NAS over Wi-Fi, is buffered before it plays rather than
// played in pieces.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace camel::audio {

// Sources needing less than this are not measured, so ordinary files start as soon as before.
inline constexpr double kMeasuredBytesPerSecond = 1'000'000.0;
// Buffering stops once this many seconds are buffered for every second a fast enough source plays.
inline constexpr double kFastEnough = 1.1;

bool onNetworkShare(const std::wstring& path);

// Reads `bytes` from `offset` past Windows' file cache, which would otherwise report the speed of memory, and returns
// the bytes read per second; nothing when the file cannot be read.
std::optional<double> measureReadRate(const std::wstring& path, uint64_t offset, uint64_t bytes);

// The seconds to buffer before `remainingSeconds` play without running dry while the source is read at `fillRatio`
// seconds of audio per second; 0 when it reads fast enough.
double bufferSeconds(double remainingSeconds, double fillRatio);

} // namespace camel::audio
