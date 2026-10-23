// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Server/Api/ServerApi.h"
#include "Engine/PlaybackEngine.h"

namespace camel::server {

class LocalServer final : public IServer {
public:
    // An existing settings directory also hosts library.db; an omitted or missing directory disables the library.
    explicit LocalServer(const Settings& settings, std::filesystem::path settingsFile = {},
                         audio::OutputFactory factory = audio::createOutput);
    ~LocalServer() override;
    CommandStamp submit(Command command) override;
    Snapshot snapshot() const override;
    void setListener(std::function<void(const Event&)> listener) override;
    void waitIdle() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
