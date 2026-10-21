// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DeviceFormat <-> WAVEFORMATEXTENSIBLE (used by WASAPI and Kernel Streaming).
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include "Common/AudioTypes.h"

namespace camel::audio {

inline WAVEFORMATEXTENSIBLE makeWaveFormat(const DeviceFormat& f) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = WORD(f.channels);
    w.Format.nSamplesPerSec = f.sampleRate;
    w.Format.wBitsPerSample = WORD(bytesPerSample(f.type) * 8);
    w.Format.nBlockAlign = WORD(f.bytesPerFrame());
    w.Format.nAvgBytesPerSec = f.sampleRate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = WORD(validBits(f.type));
    w.dwChannelMask = f.channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w.SubFormat = f.type == SampleType::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

} // namespace camel::audio
