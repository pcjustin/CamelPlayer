// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/DecoderRegistry.h"

#include <algorithm>
#include <stdexcept>

namespace camel::audio {

void DecoderRegistry::add(DecoderDescriptor decoder) {
    std::lock_guard lock(mutex_);
    if (std::any_of(decoders_.begin(), decoders_.end(), [&](const DecoderDescriptor& d) { return d.id == decoder.id; }))
        throw std::invalid_argument("decoder already registered: " + decoder.id);
    decoders_.push_back(std::move(decoder));
}

void DecoderRegistry::setOpener(std::string_view id, Opener open) {
    std::lock_guard lock(mutex_);
    for (auto& d : decoders_)
        if (d.id == id) {
            d.open = std::move(open);
            return;
        }
    throw std::invalid_argument("unknown decoder: " + std::string(id));
}

void DecoderRegistry::setDefault(std::string_view format, std::string_view decoder) {
    std::lock_guard lock(mutex_);
    for (auto& [f, d] : defaults_)
        if (f == format) {
            d = decoder;
            return;
        }
    defaults_.emplace_back(format, decoder);
}

void DecoderRegistry::addProbe(ProbeDescriptor probe) {
    std::lock_guard lock(mutex_);
    probes_.push_back(std::move(probe));
}

std::vector<DecoderDescriptor> DecoderRegistry::forFormat(std::string_view format) const {
    std::lock_guard lock(mutex_);
    std::vector<DecoderDescriptor> out;
    for (const auto& d : decoders_)
        if (std::find(d.formats.begin(), d.formats.end(), format) != d.formats.end()) out.push_back(d);
    return out;
}

std::optional<DecoderDescriptor> DecoderRegistry::find(std::string_view id) const {
    std::lock_guard lock(mutex_);
    for (const auto& d : decoders_)
        if (d.id == id) return d;
    return std::nullopt;
}

std::optional<std::string> DecoderRegistry::defaultFor(std::string_view format) const {
    std::lock_guard lock(mutex_);
    for (const auto& [f, d] : defaults_)
        if (f == format) return d;
    return std::nullopt;
}

std::vector<ProbeDescriptor> DecoderRegistry::probes() const {
    std::lock_guard lock(mutex_);
    return probes_;
}

std::optional<size_t> DecoderRegistry::indexOf(std::string_view id) const {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < decoders_.size(); i++)
        if (decoders_[i].id == id) return i;
    return std::nullopt;
}

std::optional<DecoderDescriptor> DecoderRegistry::at(size_t index) const {
    std::lock_guard lock(mutex_);
    if (index >= decoders_.size()) return std::nullopt;
    return decoders_[index];
}

namespace {
struct Builtins {
    DecoderRegistry r;
    Builtins() {
        const auto builtin = [&](DecoderKind kind, std::vector<std::string> formats) {
            r.add({decoderId(kind), decoderName(kind), std::move(formats), kind, {}});
        };
        // Registration order is the order settings list decoders in.
        builtin(DecoderKind::FFmpeg, {"flac", "alac", "wav", "aiff", "aac", "mp3"});
        builtin(DecoderKind::Symphonia, {"flac", "alac", "wav", "aiff", "aac", "mp3"});
        builtin(DecoderKind::DrFlac, {"flac"});
        builtin(DecoderKind::Sndfile, {"flac", "wav", "aiff"});
        builtin(DecoderKind::DrWav, {"wav", "aiff"});
        builtin(DecoderKind::Mpg123, {"mp3"});
        builtin(DecoderKind::DrMp3, {"mp3"});
        builtin(DecoderKind::DsdNexus, {"dsd"});
        for (const char* format : {"flac", "wav", "aiff"}) r.setDefault(format, decoderId(DecoderKind::Sndfile));
        for (const char* format : {"alac", "aac"}) r.setDefault(format, decoderId(DecoderKind::Symphonia));
        r.setDefault("mp3", decoderId(DecoderKind::Mpg123));
        r.setDefault("dsd", decoderId(DecoderKind::DsdNexus));
    }
};
} // namespace

DecoderRegistry& decoderRegistry() {
    static Builtins builtins;
    return builtins.r;
}

} // namespace camel::audio
