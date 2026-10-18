#pragma once
#include <cstdint>
#include <string_view>

namespace camel::audio {

// DsdNexus reads DSF/DSDIFF as raw DSD for DoP/native output; it is not a PCM decoder.
enum class DecoderKind : uint8_t { FFmpeg, Symphonia, DrFlac, Sndfile, DrWav, Mpg123, DrMp3, DsdNexus };
constexpr DecoderKind kAllDecoders[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::DrFlac, DecoderKind::Sndfile,
                                        DecoderKind::DrWav,  DecoderKind::Mpg123,    DecoderKind::DrMp3,  DecoderKind::DsdNexus};

const char* decoderName(DecoderKind kind);
const char* decoderId(DecoderKind kind);
bool decoderFromId(std::string_view id, DecoderKind& out);

} // namespace camel::audio
