// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/RemoteAccess.h"

#include <windows.h>

#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Api/Caller.h"
#include "Server/Remote/Discovery.h"

namespace camel::server {

struct RemoteAccess::Impl {
    Impl(LocalServer& s, Options o) : server(s), options(std::move(o)), id(serverIdIn(options.data)) {}

    LocalServer& server;
    Options options;
    std::string id;
    uint64_t observer = 0;

    std::mutex wantMutex;
    std::condition_variable wake;
    bool wanted = false, stopping = false;

    mutable std::mutex apiMutex;
    std::unique_ptr<ApiServer> api;
    std::unique_ptr<ServerAnnouncer> announcer;
    std::jthread worker;

    void want(bool allowed) {
        {
            std::lock_guard lock(wantMutex);
            if (wanted == allowed) return;
            wanted = allowed;
        }
        wake.notify_all();
    }

    // Opening and closing happen here rather than on the Server worker, whose observers must not wait.
    void run() {
        bool serving = false;
        for (;;) {
            bool allowed = false;
            {
                std::unique_lock lock(wantMutex);
                wake.wait(lock, [&] { return stopping || wanted != serving; });
                if (stopping) break;
                allowed = wanted;
            }
            serving = allowed;
            if (allowed)
                open();
            else
                close();
        }
        close();
    }

    void open() {
        ApiServer::Options served;
        served.port = options.port;
        served.loopbackOnly = options.loopbackOnly;
        served.serverId = id;
        served.serverName = options.name;
        served.appVersion = options.appVersion;
        served.calls = [this](const std::string& call, const nlohmann::json&) { return updateCall(call); };
        if (!options.data.empty()) {
            served.uploads = options.data / L"uploads";
            served.pairings = options.data / L"pairings.json";
        }
        try {
            auto opened = std::make_unique<ApiServer>(server, served);
            std::unique_ptr<ServerAnnouncer> announcing;
            if (options.announce)
                announcing = std::make_unique<ServerAnnouncer>(ServerAnnouncement{id, options.name, opened->port()});
            log::info("Server API: other computers may control this one, on port %u", unsigned(opened->port()));
            std::lock_guard lock(apiMutex);
            api = std::move(opened);
            announcer = std::move(announcing);
        } catch (const std::exception& e) {
            log::error("Server API: cannot be served: %s", e.what());
        }
    }

    void close() {
        std::unique_ptr<ApiServer> closing;
        std::unique_ptr<ServerAnnouncer> silent;
        {
            std::lock_guard lock(apiMutex);
            closing = std::move(api);
            silent = std::move(announcer);
        }
        if (!closing) return;
        silent.reset();
        closing.reset();
        log::info("Server API: other computers may no longer control this one");
    }

    template <class F> auto withApi(F&& f) const {
        std::lock_guard lock(apiMutex);
        if (!api) throw std::runtime_error("Server API: other computers may not control this one");
        return f(*api);
    }

    std::mutex installMutex;
    Install install;

    // Whether only an administrator may change the program folder, as under Program Files.
    bool needsAdministrator() const {
        if (options.programDirectory.empty()) return false;
        const auto probe = options.programDirectory / L"camelplayer-update-check";
        const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_ACCESS_DENIED;
        CloseHandle(file);
        return false;
    }

    // Update Server from a Control point (docs/Plan.md §17.8): the Server looks for the release on GitHub itself,
    // says what stops, and installs it with its own updater.
    std::optional<ApiSession::Answer> updateCall(const std::string& call) {
        if (call == "serverUpdate") {
            nlohmann::json state{{"version", options.appVersion},
                                 {"release", nullptr},
                                 {"administrator", needsAdministrator()},
                                 {"playing", nlohmann::json::array()},
                                 {"connected", nlohmann::json::array()}};
            if (const auto release = update::check(options.appVersion))
                state["release"] = {{"version", release->version}, {"notes", release->notes}};
            for (const auto& zone : server.snapshot().zones)
                if (zone.playback.state == PlaybackState::Playing) state["playing"].push_back(zone.name);
            for (const auto& device : withApi([](ApiServer& api) { return api.pairedDevices(); }))
                if (device.value("connected", false) && device.value("id", std::string()) != currentCaller().device)
                    state["connected"].push_back(device.value("name", std::string()));
            return ApiSession::Answer{std::move(state), {}};
        }
        if (call == "updateServer") {
            const auto release = update::check(options.appVersion);
            if (!release) throw std::invalid_argument("CamelPlayer is up to date");
            if (needsAdministrator()) throw std::logic_error("the program folder needs an administrator");
            Install installing;
            {
                std::lock_guard lock(installMutex);
                installing = install;
            }
            if (!installing) throw std::logic_error("CamelPlayer cannot install an update now");
            log::info("update: %s asked for %s", currentCaller().name.c_str(), release->version.c_str());
            auto prepared = update::prepare(*release, {}, {});
            return ApiSession::Answer{nlohmann::json{{"version", release->version}},
                                      [installing, release = *release, prepared] { installing(release, prepared); }};
        }
        return std::nullopt;
    }
};

RemoteAccess::RemoteAccess(LocalServer& server, Options options) : impl_(std::make_unique<Impl>(server, std::move(options))) {
    impl_->observer =
        server.addObserver([impl = impl_.get()](const Event& event) { impl->want(event.snapshot.settings.allowControl); });
    impl_->want(server.snapshot().settings.allowControl);
    impl_->worker = std::jthread([impl = impl_.get()] { impl->run(); });
}

RemoteAccess::~RemoteAccess() {
    stop();
}

void RemoteAccess::stop() {
    if (!impl_->worker.joinable()) return;
    impl_->server.removeObserver(impl_->observer);
    {
        std::lock_guard lock(impl_->wantMutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    impl_->worker.join();
}

std::optional<uint16_t> RemoteAccess::port() const {
    std::lock_guard lock(impl_->apiMutex);
    if (!impl_->api) return std::nullopt;
    return impl_->api->port();
}

const std::string& RemoteAccess::serverId() const {
    return impl_->id;
}

void RemoteAccess::setInstall(Install install) {
    std::lock_guard lock(impl_->installMutex);
    impl_->install = std::move(install);
}

std::string RemoteAccess::pairingCode() {
    return impl_->withApi([](ApiServer& api) { return api.pairingCode(); });
}

nlohmann::json RemoteAccess::pairedDevices() {
    return impl_->withApi([](ApiServer& api) { return api.pairedDevices(); });
}

bool RemoteAccess::revokeDevice(const std::string& device) {
    return impl_->withApi([&](ApiServer& api) { return api.revoke(device); });
}

void RemoteAccess::resetOwner() {
    impl_->withApi([](ApiServer& api) {
        api.resetOwner();
        return true;
    });
}

std::string serverIdIn(const std::filesystem::path& data) {
    const auto file = data / L"server-id";
    if (!data.empty()) {
        std::ifstream in(file);
        std::string kept;
        if (in >> kept && kept.size() == 32 && kept.find_first_not_of("0123456789abcdef") == std::string::npos) return kept;
    }
    std::random_device random;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", random());
    if (!data.empty()) {
        std::error_code error;
        std::filesystem::create_directories(data, error);
        std::ofstream(file) << id;
    }
    return id;
}

std::string computerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    auto size = DWORD(std::size(name));
    if (!GetComputerNameW(name, &size)) return "CamelPlayer";
    return toUtf8(std::wstring(name, size));
}

} // namespace camel::server
