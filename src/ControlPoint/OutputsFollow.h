// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>

#include "Server/Api/ServerApi.h"

namespace ui {

enum class OutputsRefill : uint8_t { None, Outputs, Settings };

// The outputs a device scan changed on the Server, and the parameters it can move with the Windows default, reach
// `target`, the settings the window keeps. Returns what of the page must be filled again: nothing while an output's
// settings dialog is open, as the page is filled once it closes.
inline OutputsRefill followServerOutputs(camel::server::SettingsDto& target, const camel::server::SettingsDto& server,
                                         bool dialogOpen) {
    const bool parameters = server.backend != target.backend || server.wasapi != target.wasapi || server.asio != target.asio ||
                            server.ks != target.ks || server.volume.mode != target.volume.mode;
    target.outputs = server.outputs;
    target.followedDefault = server.followedDefault;
    target.backend = server.backend;
    target.wasapi = server.wasapi;
    target.asio = server.asio;
    target.ks = server.ks;
    target.volume.mode = server.volume.mode;
    target.airplay = server.airplay;
    target.airplayGroup = server.airplayGroup;
    if (dialogOpen) return OutputsRefill::None;
    return parameters ? OutputsRefill::Settings : OutputsRefill::Outputs;
}

} // namespace ui
