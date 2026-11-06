// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <cstdint>
#include <string_view>

namespace camel::audio {

// DsdNexus reads DSF/DSDIFF as raw DSD for DoP/native output; it is not a PCM decoder. Values are stored, so a new
// decoder comes last and a removed one's value is not used again: 0 was FFmpeg, 6 dr_mp3 and 8 minimp3.
enum class DecoderKind : uint8_t { Symphonia = 1, DrFlac, Sndfile, DrWav, Mpg123, DsdNexus = 7 };
constexpr DecoderKind kAllDecoders[] = {DecoderKind::Symphonia, DecoderKind::DrFlac, DecoderKind::Sndfile,
                                        DecoderKind::DrWav,     DecoderKind::Mpg123, DecoderKind::DsdNexus};

const char* decoderName(DecoderKind kind);
const char* decoderId(DecoderKind kind);
bool decoderFromId(std::string_view id, DecoderKind& out);

} // namespace camel::audio
