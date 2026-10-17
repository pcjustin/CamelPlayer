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

} // namespace camel::audio
