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
    // Default first.
    static constexpr DecoderKind kFlac[] = {DecoderKind::Sndfile, DecoderKind::FFmpeg, DecoderKind::Symphonia,
                                            DecoderKind::DrFlac};
    if (format == AudioFormat::Flac) return kFlac;
    return {};
}

bool supports(AudioFormat format, DecoderKind decoder) {
    const auto decoders = decodersFor(format);
    return std::find(decoders.begin(), decoders.end(), decoder) != decoders.end();
}

DecoderKind defaultDecoder(AudioFormat format) {
    return decodersFor(format).front();
}

} // namespace camel::audio
