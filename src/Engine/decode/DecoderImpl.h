// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Constructors of the concrete decoders; only Decoders.cpp calls these. `lossyBits` (16 or 24) is the depth lossy
// streams are quantized to; lossless streams keep their source depth.
#pragma once
#include "Engine/decode/IDecoder.h"

namespace camel::audio {

// `start` is where FFmpeg and dr_mp3 begin reading the file: an MP3's first frame (FormatProbe.h, mpegAudioStart).
std::unique_ptr<IDecoder> openFfmpeg(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start = 0);
std::unique_ptr<IDecoder> openSymphonia(const std::filesystem::path& path, uint32_t lossyBits);
std::unique_ptr<IDecoder> openDrFlac(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openSndfile(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openDrWav(const std::filesystem::path& path);
std::unique_ptr<IDecoder> openMpg123(const std::filesystem::path& path, uint32_t lossyBits);
std::unique_ptr<IDecoder> openDrMp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start = 0);
// Raw DSD: SourceInfo counts byte frames (DSD rate / 8) and reports 1 bit per sample; read() returns one byte
// per channel per frame, oldest sample in the MSB.
std::unique_ptr<IDecoder> openDsdNexus(const std::filesystem::path& path);
// Decodes the DTS bitstream that `words`, a 16-bit stereo WAVE decoder, reads from a DTS-CD file.
std::unique_ptr<IDecoder> openDtsStage(std::unique_ptr<IDecoder> words, uint32_t lossyBits);

} // namespace camel::audio
