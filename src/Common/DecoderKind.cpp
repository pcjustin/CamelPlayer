// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/DecoderKind.h"

namespace camel::audio {

const char* decoderName(DecoderKind k) {
    switch (k) {
    case DecoderKind::Symphonia:
        return "Symphonia";
    case DecoderKind::DrFlac:
        return "dr_flac";
    case DecoderKind::Sndfile:
        return "libsndfile";
    case DecoderKind::DrWav:
        return "dr_wav";
    case DecoderKind::Mpg123:
        return "mpg123";
    case DecoderKind::DsdNexus:
        return "DSD Nexus";
    }
    return "?";
}

const char* decoderId(DecoderKind k) {
    switch (k) {
    case DecoderKind::Symphonia:
        return "symphonia";
    case DecoderKind::DrFlac:
        return "drflac";
    case DecoderKind::Sndfile:
        return "sndfile";
    case DecoderKind::DrWav:
        return "drwav";
    case DecoderKind::Mpg123:
        return "mpg123";
    case DecoderKind::DsdNexus:
        return "dsdnexus";
    }
    return "?";
}

bool decoderFromId(std::string_view id, DecoderKind& out) {
    for (DecoderKind k : kAllDecoders)
        if (id == decoderId(k)) {
            out = k;
            return true;
        }
    return false;
}

} // namespace camel::audio
