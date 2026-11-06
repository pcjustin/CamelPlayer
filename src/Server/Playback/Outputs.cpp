// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/Outputs.h"

#include <algorithm>

namespace camel::server {

namespace {

const SavedOutput* findOutput(const Settings& s, Backend backend, const std::string& id) {
    const auto found = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    return found == s.outputs.end() ? nullptr : &*found;
}

SavedOutput newOutput(Backend backend) {
    SavedOutput added;
    added.backend = backend;
    if (isNetworkPlayer(backend)) added.volumeMode = VolumeMode::Player;
    return added;
}

SavedOutput& savedOutput(Settings& s, Backend backend, const std::string& id) {
    const auto found = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    if (found != s.outputs.end()) return *found;
    SavedOutput added = newOutput(backend);
    added.id = id;
    added.wasapi.deviceId = added.asio.driverClsid = added.ks.deviceId = added.network.udn = added.airplay.deviceId = id;
    return s.outputs.emplace_back(std::move(added));
}

const std::string& selectedId(const Settings& s) {
    return s.backend == Backend::Asio      ? s.asio.driverClsid
           : s.backend == Backend::Ks      ? s.ks.deviceId
           : s.backend == Backend::Network ? s.network.udn
           : s.backend == Backend::AirPlay ? s.airplay.deviceId
                                           : s.wasapi.deviceId;
}

bool followsDefault(const Settings& s) {
    return s.backend == Backend::Wasapi && s.wasapi.deviceId.empty();
}

// The device whose parameters are in Settings' backend fields.
const std::string& deviceInUse(const Settings& s) {
    return followsDefault(s) ? s.followedDefault : selectedId(s);
}

void keepParameters(Settings& s, Backend backend, const std::string& id) {
    if (id.empty()) return;
    SavedOutput& saved = savedOutput(s, backend, id);
    saved.volumeMode = s.volume.mode;
    if (backend == Backend::Asio)
        saved.asio = s.asio;
    else if (backend == Backend::Ks)
        saved.ks = s.ks;
    else if (backend == Backend::Network)
        saved.network = s.network;
    else if (backend == Backend::AirPlay)
        saved.airplay = s.airplay;
    else
        saved.wasapi = s.wasapi;
    saved.wasapi.deviceId = saved.asio.driverClsid = saved.ks.deviceId = saved.network.udn = saved.airplay.deviceId = id;
}

// Puts `device`'s parameters, or the defaults for a device without any, in Settings' fields for the output `selected`.
void loadParameters(Settings& s, Backend backend, const std::string& device, std::string selected) {
    const SavedOutput* saved = device.empty() ? nullptr : findOutput(s, backend, device);
    const SavedOutput defaults = newOutput(backend);
    const SavedOutput& source = saved ? *saved : defaults;
    s.backend = backend;
    s.volume.mode = source.volumeMode;
    if (isNetworkPlayer(backend) && s.volume.mode == VolumeMode::Software) s.volume.mode = VolumeMode::Player;
    if (backend == Backend::Asio) {
        s.asio = source.asio;
        s.asio.driverClsid = std::move(selected);
    } else if (backend == Backend::Ks) {
        s.ks = source.ks;
        s.ks.deviceId = std::move(selected);
    } else if (backend == Backend::Network) {
        s.network = source.network;
        s.network.udn = std::move(selected);
    } else if (backend == Backend::AirPlay) {
        s.airplay = source.airplay;
        s.airplay.deviceId = std::move(selected);
    } else {
        s.wasapi = source.wasapi;
        s.wasapi.deviceId = std::move(selected);
    }
}

// "Windows default device" keeps its parameters under the device it stands for now.
std::string parameterDevice(const DeviceCatalog& devices, Backend backend, const std::string& id) {
    return backend == Backend::Wasapi && id.empty() ? devices.defaultWasapi : id;
}

// Each backend's set goes to the device it had selected; empty meant the Windows default, or the first ASIO driver or
// KS pin (docs/Plan.md §6).
void adoptSets(Settings& s, const DeviceCatalog& devices) {
    if (!s.adoptOutputSets) return;
    s.adoptOutputSets = false;
    if (s.asio.driverClsid.empty() && !devices.asio.empty()) s.asio.driverClsid = devices.asio.front().id;
    if (s.ks.deviceId.empty() && !devices.ks.empty()) s.ks.deviceId = devices.ks.front().id;
    keepParameters(s, Backend::Wasapi, s.wasapi.deviceId.empty() ? devices.defaultWasapi : s.wasapi.deviceId);
    keepParameters(s, Backend::Asio, s.asio.driverClsid);
    keepParameters(s, Backend::Ks, s.ks.deviceId);
    if (followsDefault(s)) s.followedDefault = devices.defaultWasapi;
}

const std::vector<DeviceInfo>& devicesOf(const DeviceCatalog& devices, Backend backend) {
    return backend == Backend::Asio      ? devices.asio
           : backend == Backend::Ks      ? devices.ks
           : backend == Backend::Network ? devices.network
           : backend == Backend::AirPlay ? devices.airplay
                                         : devices.wasapi;
}

const DeviceInfo* findDevice(const DeviceCatalog& devices, Backend backend, const std::string& id) {
    const auto& list = devicesOf(devices, backend);
    const auto found = std::ranges::find_if(list, [&](const DeviceInfo& d) { return d.id == id; });
    return found == list.end() ? nullptr : &*found;
}

} // namespace

std::vector<OutputRow> outputRows(const Settings& s, const DeviceCatalog& devices) {
    std::vector<OutputRow> rows;
    const auto add = [&](Backend backend, const std::string& id, const std::string& deviceName, bool connected,
                         const std::string& unsupported) {
        OutputRow row;
        row.backend = backend;
        row.id = id;
        row.connected = connected;
        row.unsupported = unsupported;
        row.selected = s.backend == backend && selectedId(s) == id;
        const SavedOutput* saved = findOutput(s, backend, id);
        row.saved = saved != nullptr && !id.empty();
        row.enabled = saved && saved->enabled;
        row.alias = saved ? saved->alias : std::string();
        row.name = !row.alias.empty() ? row.alias : saved && deviceName.empty() ? saved->name : deviceName;
        rows.push_back(std::move(row));
    };
    for (const Backend backend : {Backend::Wasapi, Backend::Asio, Backend::Ks, Backend::Network, Backend::AirPlay}) {
        if (backend == Backend::Wasapi) add(backend, {}, {}, true, {});
        for (const auto& device : devicesOf(devices, backend)) add(backend, device.id, device.name, true, device.unsupported);
        for (const auto& saved : s.outputs)
            if (saved.backend == backend && !saved.id.empty() && !findDevice(devices, backend, saved.id))
                add(backend, saved.id, {}, devices.revision == 0, {});
    }
    return rows;
}

void syncOutputs(Settings& s, const DeviceCatalog& devices) {
    if (devices.revision == 0) return;
    adoptSets(s, devices);
    if (followsDefault(s) && !devices.defaultWasapi.empty() && s.followedDefault != devices.defaultWasapi) {
        keepParameters(s, Backend::Wasapi, s.followedDefault);
        loadParameters(s, Backend::Wasapi, devices.defaultWasapi, {});
        s.followedDefault = devices.defaultWasapi;
    }
    keepParameters(s, s.backend, deviceInUse(s));
    if (s.backend == Backend::Wasapi || !selectedId(s).empty()) savedOutput(s, s.backend, selectedId(s)).enabled = true;
    for (auto& saved : s.outputs)
        if (const auto* device = findDevice(devices, saved.backend, saved.id)) {
            saved.name = device->name;
            const bool inUse = saved.backend == s.backend && saved.id == selectedId(s);
            if (!device->unsupported.empty() && !inUse) saved.enabled = false;
        }
}

Settings withOutputParameters(Settings s, Backend backend, const std::string& id, const DeviceCatalog& devices) {
    if (s.backend == backend && selectedId(s) == id) return s;
    const std::string device = parameterDevice(devices, backend, id);
    loadParameters(s, backend, device, id);
    return s;
}

void keepOutputParameters(Settings& s, const Settings& edited, Backend backend, const std::string& id,
                          const DeviceCatalog& devices) {
    const bool inUse = s.backend == backend && selectedId(s) == id;
    if (inUse) {
        if (backend == Backend::Asio)
            s.asio = edited.asio;
        else if (backend == Backend::Ks)
            s.ks = edited.ks;
        else if (backend == Backend::Network)
            s.network = edited.network;
        else if (backend == Backend::AirPlay)
            s.airplay = edited.airplay;
        else
            s.wasapi = edited.wasapi;
        return;
    }
    const std::string device = parameterDevice(devices, backend, id);
    if (device.empty()) return;
    Settings kept = edited;
    kept.outputs = s.outputs;
    const SavedOutput* saved = findOutput(s, backend, device);
    kept.volume.mode = saved ? saved->volumeMode : newOutput(backend).volumeMode;
    keepParameters(kept, backend, device);
    s.outputs = std::move(kept.outputs);
}

void selectOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices) {
    keepParameters(s, s.backend, deviceInUse(s));
    const bool following = backend == Backend::Wasapi && id.empty();
    loadParameters(s, backend, following ? devices.defaultWasapi : id, id);
    s.followedDefault = following ? devices.defaultWasapi : std::string();
    savedOutput(s, backend, id).enabled = true;
    syncOutputs(s, devices);
}

void setOutputEnabled(Settings& s, Backend backend, const std::string& id, bool enabled, const DeviceCatalog& devices) {
    if (s.backend == backend && selectedId(s) == id) return;
    savedOutput(s, backend, id).enabled = enabled;
    syncOutputs(s, devices);
}

void renameOutput(Settings& s, Backend backend, const std::string& id, const std::string& alias, const DeviceCatalog& devices) {
    savedOutput(s, backend, id).alias = alias;
    syncOutputs(s, devices);
}

void deleteOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices) {
    if (id.empty()) return;
    std::erase_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    if (s.backend == backend && deviceInUse(s) == id) loadParameters(s, backend, id, selectedId(s));
    syncOutputs(s, devices);
}

} // namespace camel::server
