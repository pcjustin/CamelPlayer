// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Identifies a file's container and codec from its contents; the extension is ignored (docs/Plan.md §3.1).
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>

#include "Common/AudioFormat.h"

namespace camel::audio {

// Throws Error (FileOpen when unreadable, UnsupportedStream when unrecognized or an unsupported codec/profile).
AudioFormat probeFormat(const std::filesystem::path& path);

// Whether a lossy file declares its length: an MP4 container, or an MP3 Xing/Info/VBRI header. Raw ADTS and plain MP3
// streams do not, so decoders can only estimate it.
bool lossyLengthDeclared(const std::filesystem::path& path);

// numSampleFrames of an AIFF/AIFC COMM chunk; nullopt when the file has none.
std::optional<uint64_t> aiffSampleFrames(const std::filesystem::path& path);

// Encoder delay and valid length an MP4 declares for its audio track, in frames at `sampleRate`: the edit list, or else
// iTunes' iTunSMPB tag. nullopt when the file declares neither (or more than one edit), so nothing can be trimmed.
struct EncoderTrim {
    uint64_t priming = 0;
    uint64_t frames = 0;
};
std::optional<EncoderTrim> mp4EncoderTrim(const std::filesystem::path& path, uint32_t sampleRate);

} // namespace camel::audio
