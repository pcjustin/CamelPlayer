// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/AudioFormat.h"

#include <algorithm>

namespace camel::audio {

const char* formatId(AudioFormat format) {
    switch (format) {
    case AudioFormat::Flac:
        return "flac";
    case AudioFormat::Alac:
        return "alac";
    case AudioFormat::Wav:
        return "wav";
    case AudioFormat::Aiff:
        return "aiff";
    case AudioFormat::Aac:
        return "aac";
    case AudioFormat::Mp3:
        return "mp3";
    case AudioFormat::Dsd:
        return "dsd";
    }
    return "?";
}

const char* formatName(AudioFormat format) {
    switch (format) {
    case AudioFormat::Flac:
        return "FLAC";
    case AudioFormat::Alac:
        return "ALAC";
    case AudioFormat::Wav:
        return "WAV";
    case AudioFormat::Aiff:
        return "AIFF";
    case AudioFormat::Aac:
        return "AAC";
    case AudioFormat::Mp3:
        return "MP3";
    case AudioFormat::Dsd:
        return "DSD";
    }
    return "?";
}

bool formatFromId(std::string_view id, AudioFormat& out) {
    for (AudioFormat f : kAllFormats)
        if (id == formatId(f)) {
            out = f;
            return true;
        }
    return false;
}

std::span<const DecoderKind> decodersFor(AudioFormat format) {
    static constexpr DecoderKind kFlac[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::DrFlac,
                                            DecoderKind::Sndfile};
    static constexpr DecoderKind kAlac[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia};
    static constexpr DecoderKind kPcm[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::Sndfile, DecoderKind::DrWav};
    static constexpr DecoderKind kAac[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia};
    static constexpr DecoderKind kMp3[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::Mpg123, DecoderKind::DrMp3};
    static constexpr DecoderKind kDsd[] = {DecoderKind::DsdNexus};
    switch (format) {
    case AudioFormat::Flac:
        return kFlac;
    case AudioFormat::Alac:
        return kAlac;
    case AudioFormat::Aac:
        return kAac;
    case AudioFormat::Mp3:
        return kMp3;
    case AudioFormat::Wav:
    case AudioFormat::Aiff:
        return kPcm;
    case AudioFormat::Dsd:
        return kDsd;
    default:
        return {};
    }
}

bool supports(AudioFormat format, DecoderKind decoder) {
    const auto decoders = decodersFor(format);
    return std::find(decoders.begin(), decoders.end(), decoder) != decoders.end();
}

DecoderKind defaultDecoder(AudioFormat format) {
    switch (format) {
    case AudioFormat::Alac:
    case AudioFormat::Aac:
        return DecoderKind::Symphonia;
    case AudioFormat::Mp3:
        return DecoderKind::Mpg123;
    case AudioFormat::Dsd:
        return DecoderKind::DsdNexus;
    default:
        return DecoderKind::Sndfile;
    }
}

bool isLossy(AudioFormat format) {
    return format == AudioFormat::Aac || format == AudioFormat::Mp3;
}

} // namespace camel::audio
