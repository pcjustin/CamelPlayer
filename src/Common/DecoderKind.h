#pragma once
#include <cstdint>
#include <string_view>

namespace camel::audio {

enum class DecoderKind : uint8_t { FFmpeg, Symphonia, DrFlac, Sndfile };
constexpr DecoderKind kAllDecoders[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::DrFlac, DecoderKind::Sndfile};

const char* decoderName(DecoderKind kind);
const char* decoderId(DecoderKind kind);
bool decoderFromId(std::string_view id, DecoderKind& out);

} // namespace camel::audio
