// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Decoders and format probes by string ID (docs/Extension-points.md #1, #2). Built-ins keep the IDs settings already
// store (decoderId(), formatId()); extension decoders will use "<package>/<id>".
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/DecoderKind.h"

namespace camel::audio {

class IDecoder;

struct DecoderDescriptor {
    std::string id;
    std::string name;
    std::vector<std::string> formats; // formatId() values, in no particular order
    std::optional<DecoderKind> builtin;
    // Opens `path` (already probed as `format`); lossy formats decode to `lossyBits`. Set by the Engine for built-ins.
    std::function<std::unique_ptr<IDecoder>(const std::filesystem::path& path, std::string_view format, uint32_t lossyBits)> open;
};

// Returns the format ID of `path`, or nullopt when it does not recognize the file. May throw Error for a recognized
// stream that cannot be played.
struct ProbeDescriptor {
    std::string id;
    std::function<std::optional<std::string>(const std::filesystem::path& path)> identify;
};

class DecoderRegistry {
public:
    using Opener = decltype(DecoderDescriptor::open);

    void add(DecoderDescriptor decoder); // throws std::invalid_argument for a duplicate ID
    void setOpener(std::string_view id, Opener open);
    void setDefault(std::string_view format, std::string_view decoder);
    void addProbe(ProbeDescriptor probe); // consulted in the order added

    std::vector<DecoderDescriptor> forFormat(std::string_view format) const; // in registration order
    std::optional<DecoderDescriptor> find(std::string_view id) const;
    std::optional<std::string> defaultFor(std::string_view format) const;
    std::vector<ProbeDescriptor> probes() const;
    // Registration index, stable for the process; the C ABI decoder host numbers registered decoders by it.
    std::optional<size_t> indexOf(std::string_view id) const;
    std::optional<DecoderDescriptor> at(size_t index) const;

private:
    mutable std::mutex mutex_;
    std::vector<DecoderDescriptor> decoders_;
    std::vector<std::pair<std::string, std::string>> defaults_;
    std::vector<ProbeDescriptor> probes_;
};

// The process-wide registry with the built-in decoders, their formats and defaults (docs/Plan.md §3.1).
DecoderRegistry& decoderRegistry();

} // namespace camel::audio
