// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/AudioFormat.h"

#include <algorithm>

#include "Common/DecoderRegistry.h"

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

std::vector<DecoderKind> decodersFor(AudioFormat format) {
    std::vector<DecoderKind> out;
    for (const auto& d : decoderRegistry().forFormat(formatId(format)))
        if (d.builtin) out.push_back(*d.builtin);
    return out;
}

bool supports(AudioFormat format, DecoderKind decoder) {
    const auto decoders = decodersFor(format);
    return std::find(decoders.begin(), decoders.end(), decoder) != decoders.end();
}

DecoderKind defaultDecoder(AudioFormat format) {
    const auto id = decoderRegistry().defaultFor(formatId(format));
    DecoderKind kind{};
    if (!id || !decoderFromId(*id, kind)) return DecoderKind::Sndfile;
    return kind;
}

bool isLossy(AudioFormat format) {
    return format == AudioFormat::Aac || format == AudioFormat::Mp3;
}

} // namespace camel::audio
