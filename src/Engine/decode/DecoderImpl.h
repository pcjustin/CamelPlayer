// Constructors of the concrete decoders; only Decoders.cpp calls these. `lossyBits` (16 or 24) is the depth lossy
// streams are quantized to; lossless streams keep their source depth.
#pragma once
#include "Engine/decode/IDecoder.h"

namespace camel::audio {

std::unique_ptr<IDecoder> openFfmpeg(const std::filesystem::path& path, uint32_t lossyBits);
std::unique_ptr<IDecoder> openSymphonia(const std::filesystem::path& path, uint32_t lossyBits);
std::unique_ptr<IDecoder> openDrFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSndfile(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openDrWav(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openMpg123(const std::filesystem::path& path, uint32_t lossyBits);
std::unique_ptr<IDecoder> openDrMp3(const std::filesystem::path& path, uint32_t lossyBits);

} // namespace camel::audio
