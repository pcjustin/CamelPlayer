// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Lossless JSON form of the Server API (docs/Extension-points.md #4), ready for a network transport. Enums are written
// as stable string IDs (decoders and formats as in settings). Decoding is strict: a missing field, a wrong type or an
// unknown ID throws std::invalid_argument.
#pragma once
#include <nlohmann/json.hpp>

#include "Server/Api/ServerApi.h"

namespace camel::server {

nlohmann::json toJson(const Command& command);
Command commandFromJson(const nlohmann::json& j);

nlohmann::json toJson(const CommandStamp& stamp);
CommandStamp commandStampFromJson(const nlohmann::json& j);

nlohmann::json toJson(const Snapshot& snapshot);
Snapshot snapshotFromJson(const nlohmann::json& j);

nlohmann::json toJson(const Event& event);
Event eventFromJson(const nlohmann::json& j);

} // namespace camel::server
