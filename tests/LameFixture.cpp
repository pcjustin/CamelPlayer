// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "LameFixture.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

#include <lame/lame.h>

namespace testutil {

void writeMp3(const std::filesystem::path& path, const std::vector<int32_t>& samples, uint32_t sampleRate, uint32_t bits,
              uint32_t channels, bool lameTag) {
    if ((bits != 16 && bits != 24) || (channels != 1 && channels != 2) || samples.size() % channels)
        throw std::invalid_argument("MP3 test files take 16/24-bit mono or stereo samples");
    const std::unique_ptr<lame_global_flags, decltype(&lame_close)> lame(lame_init(), lame_close);
    if (!lame) throw std::runtime_error("lame_init failed");
    lame_set_in_samplerate(lame.get(), int(sampleRate));
    lame_set_out_samplerate(lame.get(), int(sampleRate));
    lame_set_num_channels(lame.get(), int(channels));
    lame_set_mode(lame.get(), channels == 1 ? MONO : JOINT_STEREO);
    lame_set_VBR(lame.get(), vbr_off);
    lame_set_brate(lame.get(), 320);
    lame_set_quality(lame.get(), 2);
    lame_set_bWriteVbrTag(lame.get(), lameTag ? 1 : 0);
    if (lame_init_params(lame.get()) < 0) throw std::runtime_error("lame_init_params failed");

    FILE* raw = nullptr;
    if (_wfopen_s(&raw, path.c_str(), L"w+b") || !raw) throw std::runtime_error("cannot create MP3 test file");
    const std::unique_ptr<FILE, decltype(&fclose)> file(raw, fclose);
    const size_t frames = samples.size() / channels;
    std::vector<int> full(samples.size());
    for (size_t i = 0; i < samples.size(); i++) full[i] = int(uint32_t(samples[i]) << (32 - bits)); // LAME takes full-scale int
    std::vector<int> left(frames), right(frames);
    for (size_t i = 0; i < frames; i++) {
        left[i] = full[i * channels];
        right[i] = full[i * channels + channels - 1];
    }
    std::vector<unsigned char> mp3(frames + frames / 4 + 7200);
    const int encoded = lame_encode_buffer_int(lame.get(), left.data(), right.data(), int(frames), mp3.data(), int(mp3.size()));
    if (encoded < 0) throw std::runtime_error("lame_encode_buffer_int failed");
    if (std::fwrite(mp3.data(), 1, size_t(encoded), file.get()) != size_t(encoded))
        throw std::runtime_error("cannot write MP3 test file");
    const int flushed = lame_encode_flush(lame.get(), mp3.data(), int(mp3.size()));
    if (flushed < 0) throw std::runtime_error("lame_encode_flush failed");
    if (std::fwrite(mp3.data(), 1, size_t(flushed), file.get()) != size_t(flushed))
        throw std::runtime_error("cannot write MP3 test file");
    if (lameTag) lame_mp3_tags_fid(lame.get(), file.get()); // rewrites the first frame with delay and padding
    if (std::ferror(file.get())) throw std::runtime_error("cannot write MP3 test file");
}

} // namespace testutil
