// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What several playback engines at once ask of this computer (docs/Plan.md §5.12): a network share's reads, which the
// engines already reading it keep, and the DSP stage's time, which the engine started last among those upsampling or
// modulating to DSD gives up when it runs short. Any thread may call these.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace camel::audio::load {

// `reader` reads `bytesPerSecond` of `share` (shareOf, SlowSource.h) from now on; 0 or an empty share ends its reads.
void setShareRead(const void* reader, const std::wstring& share, double bytesPerSecond);
// What the readers other than `reader` read of `share`, in bytes a second.
double othersRead(const void* reader, const std::wstring& share);
// The rate `share` was measured at while it is read; forgotten once nothing reads it.
std::optional<double> shareRate(const std::wstring& share);
void setShareRate(const std::wstring& share, double bytesPerSecond);

// `engine` plays, upsampling or modulating to DSD when `heavy`; `giveWay` asks it to pause, from any thread. Called
// again it keeps the time the engine started playing.
void setPlaying(const void* engine, bool heavy, const std::function<void()>& giveWay);
void setNotPlaying(const void* engine);

enum class Shortfall : uint8_t { Alone, GivingWay };
// `engine` ran short while its decoded audio waited. While others play, the engine started last among those upsampling
// or modulating to DSD is asked to give way, unless one was a moment ago: GivingWay. Alone when no other engine plays,
// or none upsamples or modulates.
Shortfall ranShort(const void* engine);

} // namespace camel::audio::load
