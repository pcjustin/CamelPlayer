// Constructors of the concrete decoders; only Decoders.cpp calls these.
#pragma once
#include "Engine/decode/IDecoder.h"

namespace camel::audio {

std::unique_ptr<IDecoder> openFfmpegFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSymphoniaFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openDrFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSndfileFlac(const std::filesystem::path& path);

} // namespace camel::audio
