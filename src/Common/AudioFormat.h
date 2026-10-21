// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Logical audio formats and the capability registry (docs/Plan.md §3.1).
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "Common/DecoderKind.h"

namespace camel::audio {

// Keyed by codec, not extension: M4A AAC and M4A ALAC are different formats.
enum class AudioFormat : uint8_t { Flac, Alac, Wav, Aiff, Aac, Mp3, Dsd };
constexpr AudioFormat kAllFormats[] = {AudioFormat::Flac, AudioFormat::Alac, AudioFormat::Wav, AudioFormat::Aiff,
                                       AudioFormat::Aac,  AudioFormat::Mp3,  AudioFormat::Dsd};

const char* formatId(AudioFormat format);
const char* formatName(AudioFormat format);
bool formatFromId(std::string_view id, AudioFormat& out);

// Integrated and tested decoders for `format`; empty while the format is not supported yet.
std::span<const DecoderKind> decodersFor(AudioFormat format);
bool supports(AudioFormat format, DecoderKind decoder);
// Precondition: decodersFor(format) is not empty.
DecoderKind defaultDecoder(AudioFormat format);

// Lossy bitstreams have no source PCM depth; their decoded output is offered as 16- or 24-bit PCM (docs/Plan.md §3.1).
bool isLossy(AudioFormat format);

} // namespace camel::audio
