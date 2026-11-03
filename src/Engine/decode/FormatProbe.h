// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Identifies a file's container and codec from its contents; the extension is ignored (docs/Plan.md §3.1).
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "Common/AudioFormat.h"
#include "Common/AudioTypes.h"

namespace camel::audio {

// Throws Error (FileOpen when unreadable, UnsupportedStream when unrecognized or an unsupported codec/profile).
AudioFormat probeFormat(const std::filesystem::path& path);

// Reads MP3 stream parameters without building a playback seek index. A stream without a declared length reports
// mpg123's size/bitrate estimate and sets `estimated`; playback's full scan supplies the exact length.
SourceInfo probeMp3Info(const std::filesystem::path& path, bool& estimated);

// Reads AAC container timing without decoding or seeking through encoder priming.
SourceInfo probeAacInfo(const std::filesystem::path& path);

// Whether a lossy file declares its length: an MP4 container, or an MP3 Xing/Info header. Raw ADTS and plain MP3
// streams do not, so decoders can only estimate it; nor does Fraunhofer's VBRI header, which mpg123 does not read.
bool lossyLengthDeclared(const std::filesystem::path& path);

// The samples of the frames an MP3's VBRI header counts after its own, 0 without one: an estimate, frames times samples
// a frame.
uint64_t vbriSamples(const std::filesystem::path& path);

// Where an MP3's sound starts: at its first frame, past its tags and any other bytes before it, or at the frame after a
// VBRI frame, which holds no sound.
uint64_t mpegSoundStart(const std::filesystem::path& path);

// What an MP3's Xing (VBR) frame declares, when it declares all three: the frames after it, the bytes from it on, and
// for each percent of the frames, where they start in 256ths of those bytes. `at` is where the frame starts in the file,
// past any tag before it.
struct XingToc {
    uint64_t at = 0;
    uint32_t frames = 0;
    uint32_t bytes = 0;
    std::array<uint8_t, 100> toc{};
    // The share of the bytes where `share` of the frames starts, between the table's entries.
    double byteShare(double share) const;
};
std::optional<XingToc> xingToc(const std::filesystem::path& path);

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
