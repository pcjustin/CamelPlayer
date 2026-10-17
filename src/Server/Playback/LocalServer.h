#pragma once

#include "Server/Api/ServerApi.h"
#include "Engine/PlaybackEngine.h"

namespace camel::server {

class LocalServer final : public IServer {
public:
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
