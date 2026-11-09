// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/Waveform.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "Engine/EngineAbi.h"
#include "Engine/decode/DsdReader.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"

namespace camel::server {

PeakCollector::PeakCollector(uint32_t sampleRate, unsigned channels)
    : blockFrames_(std::max<size_t>(1, sampleRate / 100)), channels_(std::max(1u, channels)) {}

void PeakCollector::add(const float* interleaved, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < channels_; ++c) current_ = std::max(current_, std::abs(interleaved[f * channels_ + c]));
        if (++inBlock_ == blockFrames_) {
            blocks_.push_back(current_);
            inBlock_ = 0;
            current_ = 0.0f;
        }
    }
}

std::vector<uint8_t> PeakCollector::peaks() const {
    std::vector<float> blocks = blocks_;
    if (inBlock_ > 0) blocks.push_back(current_);
    if (blocks.empty()) return {};
    std::vector<float> spread(kWaveformPeaks, 0.0f);
    for (size_t i = 0; i < kWaveformPeaks; ++i) {
        const size_t from = i * blocks.size() / kWaveformPeaks;
        const size_t to = std::max(from + 1, (i + 1) * blocks.size() / kWaveformPeaks);
        for (size_t b = from; b < to && b < blocks.size(); ++b) spread[i] = std::max(spread[i], blocks[b]);
    }
    const float loudest = *std::ranges::max_element(spread);
    std::vector<uint8_t> out(kWaveformPeaks, 0);
    if (loudest > 0.0f)
        for (size_t i = 0; i < kWaveformPeaks; ++i) out[i] = uint8_t(std::lround(std::min(1.0f, spread[i] / loudest) * 255.0f));
    return out;
}

namespace {

std::optional<std::vector<uint8_t>> readDsd(const std::filesystem::path& file, const std::function<bool()>& cancelled) {
    const auto reader = audio::openDsdReader(file);
    const auto& info = reader->info();
    const std::unique_ptr<CamelDsdToPcm, void (*)(CamelDsdToPcm*)> converter(
        camel_dsd_to_pcm_create_v1(info.sampleRate, info.channels), camel_dsd_to_pcm_destroy_v1);
    if (!converter) return std::nullopt;
    PeakCollector collector(camel_dsd_to_pcm_rate_v1(), info.channels);
    constexpr size_t kFrames = 65536;
    std::vector<uint8_t> bytes(kFrames * info.channels);
    std::vector<float> pcm(kFrames * info.channels);
    for (;;) {
        if (cancelled && cancelled()) return std::nullopt;
        const size_t frames = reader->read(bytes.data(), kFrames);
        if (frames == 0) break;
        size_t produced = 0;
        if (camel_dsd_to_pcm_process_v1(converter.get(), bytes.data(), frames, pcm.data(), kFrames, &produced) != 0)
            return std::nullopt;
        collector.add(pcm.data(), produced);
    }
    return collector.peaks();
}

} // namespace

std::optional<std::vector<uint8_t>> readWaveform(const std::filesystem::path& file, const DecoderByFormat& decoders,
                                                 const std::function<bool()>& cancelled) {
    const auto format = audio::probeFormat(file);
    if (format == audio::AudioFormat::Dsd) return readDsd(file, cancelled);
    const auto decoder = audio::openDecoder(decoderFor(decoders, format), file);
    const auto& info = decoder->info();
    if (info.channels == 0 || info.sampleRate == 0) return std::nullopt;
    PeakCollector collector(info.sampleRate, info.channels);
    constexpr size_t kFrames = 16384;
    std::vector<float> floats(kFrames * info.channels);
    std::vector<int32_t> integers(decoder->readsFloat() ? 0 : kFrames * info.channels);
    const float scale = 1.0f / float(1u << (std::clamp(info.bitsPerSample, 8u, 32u) - 1));
    uint64_t decoded = 0;
    for (;;) {
        if (cancelled && cancelled()) return std::nullopt;
        size_t frames = 0;
        try {
            if (decoder->readsFloat()) {
                frames = decoder->readFloat(floats.data(), kFrames);
            } else {
                frames = decoder->read(integers.data(), kFrames);
                for (size_t i = 0; i < frames * info.channels; ++i) floats[i] = float(integers[i]) * scale;
            }
        } catch (const audio::Error&) {
            // A FLAC whose length or MD5 disagrees with its STREAMINFO fails only at its end, as playback reports;
            // what was decoded before stands.
            if (decoded == 0) throw;
            break;
        }
        if (frames == 0) break;
        decoded += frames;
        collector.add(floats.data(), frames);
    }
    return collector.peaks();
}

void storeWaveform(sqlite::Database& db, int64_t file, const std::vector<uint8_t>& peaks) {
    db.prepare("INSERT OR REPLACE INTO waveforms(file_id,peaks) SELECT ?,? WHERE EXISTS (SELECT 1 FROM files WHERE id=?)")
        .bind(1, file)
        .bind(2, std::span<const uint8_t>(peaks))
        .bind(3, file)
        .run();
}

std::optional<std::vector<uint8_t>> storedWaveform(sqlite::Database& db, int64_t file) {
    auto row = db.prepare("SELECT peaks FROM waveforms WHERE file_id=?");
    if (!row.bind(1, file).step()) return std::nullopt;
    return row.blob(0);
}

} // namespace camel::server
