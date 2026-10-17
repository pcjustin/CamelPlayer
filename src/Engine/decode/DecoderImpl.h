// Constructors of the concrete decoders; only Decoders.cpp calls these.
#pragma once
#include "Engine/decode/IDecoder.h"

namespace camel::audio {

std::unique_ptr<IDecoder> openFfmpeg(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSymphonia(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openDrFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSndfile(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openDrWav(const std::filesystem::path& path);

} // namespace camel::audio
