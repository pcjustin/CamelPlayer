// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>

#include "Server/Api/ServerApi.h"

namespace ui {

enum class OutputsRefill : uint8_t { None, Outputs, Settings };

// The outputs a device scan changed on the Server, and the parameters it can move with the Windows default, reach
// `shown`, the settings the page shows. While an output's settings dialog shows that output's own copy, `real` holds
// the settings the dialog writes back. Returns what of the page must be filled again.
inline OutputsRefill followServerOutputs(camel::server::SettingsDto& shown, camel::server::SettingsDto* real,
                                         const camel::server::SettingsDto& server) {
    auto& target = real ? *real : shown;
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
    if (real) return OutputsRefill::None; // the page is filled from `real` once the dialog closes
    return parameters ? OutputsRefill::Settings : OutputsRefill::Outputs;
}

// The settings a change made outside an output's settings dialog is saved in: `shown`, unless the dialog is open, when
// `shown` holds that output's own copy.
inline camel::server::SettingsDto& settingsToSave(camel::server::SettingsDto& shown, camel::server::SettingsDto* real) {
    return real ? *real : shown;
}

} // namespace ui
