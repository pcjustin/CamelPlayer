// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <exception>
#include <optional>
#include <utility>

#include "Common/util/Log.h"
#include "Server/Api/ServerApi.h"

namespace ui {

// A command the Server refuses, as it checks each one, goes no further than the log: thrown into a XAML event handler,
// it ended the program.
inline std::optional<camel::server::CommandStamp> submitChecked(camel::server::IServer& server, camel::server::Command command) {
    try {
        return server.submit(std::move(command));
    } catch (const std::exception& e) {
        camel::log::error("command refused: %s", e.what());
        return std::nullopt;
    }
}

} // namespace ui
