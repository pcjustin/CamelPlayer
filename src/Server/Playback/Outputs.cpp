// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Playback/Outputs.h"

#include <algorithm>

#include "Server/Network/HQPlayer.h"

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
    added.wasapi.deviceId = added.asio.driverClsid = added.ks.deviceId = added.network.udn = added.airplay.deviceId =
        added.hqplayer.address = id;
    return s.outputs.emplace_back(std::move(added));
}

const std::string& selectedId(const Settings& s) {
    return s.backend == Backend::Asio       ? s.asio.driverClsid
           : s.backend == Backend::Ks       ? s.ks.deviceId
           : s.backend == Backend::Network  ? s.network.udn
           : s.backend == Backend::AirPlay  ? s.airplay.deviceId
           : s.backend == Backend::HQPlayer ? s.hqplayer.address
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
    saved.dsp = outputDsp(s);
    if (backend == Backend::Asio)
        saved.asio = s.asio;
    else if (backend == Backend::Ks)
        saved.ks = s.ks;
    else if (backend == Backend::Network)
        saved.network = s.network;
    else if (backend == Backend::AirPlay)
        saved.airplay = s.airplay;
    else if (backend == Backend::HQPlayer)
        saved.hqplayer = s.hqplayer;
    else
        saved.wasapi = s.wasapi;
    saved.wasapi.deviceId = saved.asio.driverClsid = saved.ks.deviceId = saved.network.udn = saved.airplay.deviceId =
        saved.hqplayer.address = id;
}

// Puts `device`'s parameters, or the defaults for a device without any, in Settings' fields for the output `selected`.
void loadParameters(Settings& s, Backend backend, const std::string& device, std::string selected) {
    const SavedOutput* saved = device.empty() ? nullptr : findOutput(s, backend, device);
    const SavedOutput defaults = newOutput(backend);
    const SavedOutput& source = saved ? *saved : defaults;
    s.backend = backend;
    s.volume.mode = source.volumeMode;
    if (isNetworkPlayer(backend) && s.volume.mode == VolumeMode::Software) s.volume.mode = VolumeMode::Player;
    setOutputDsp(s, source.dsp);
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
    } else if (backend == Backend::HQPlayer) {
        s.hqplayer = source.hqplayer;
        s.hqplayer.address = std::move(selected);
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
    return backend == Backend::Asio       ? devices.asio
           : backend == Backend::Ks       ? devices.ks
           : backend == Backend::Network  ? devices.network
           : backend == Backend::AirPlay  ? devices.airplay
           : backend == Backend::HQPlayer ? devices.hqplayer
                                          : devices.wasapi;
}

const DeviceInfo* findDevice(const DeviceCatalog& devices, Backend backend, const std::string& id) {
    const auto& list = devicesOf(devices, backend);
    const auto found = std::ranges::find_if(list, [&](const DeviceInfo& d) { return d.id == id; });
    return found == list.end() ? nullptr : &*found;
}

bool holds(const std::vector<std::string>& ids, const std::string& id) {
    return std::ranges::find(ids, id) != ids.end();
}

// What was saved for the AirPlay receiver `from` goes to `to`: the entry, or its turning on when `to` has one, the use,
// the group's place and the level.
void moveReceiver(Settings& s, const std::string& from, const std::string& to) {
    const auto saved = [&](const std::string& id) {
        return std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == Backend::AirPlay && o.id == id; });
    };
    if (const auto old = saved(from); old != s.outputs.end()) {
        if (const auto kept = saved(to); kept != s.outputs.end()) {
            kept->enabled = kept->enabled || old->enabled;
            s.outputs.erase(old);
        } else {
            old->id = old->airplay.deviceId = to;
        }
    }
    if (s.backend == Backend::AirPlay && s.airplay.deviceId == from) s.airplay.deviceId = to;
    for (auto& member : s.airplayGroup)
        if (member == from) member = to;
    std::erase_if(s.airplayGroup, [seen = std::vector<std::string>()](const std::string& id) mutable {
        if (holds(seen, id)) return true;
        seen.push_back(id);
        return false;
    });
    if (auto level = s.airplayLevels.extract(from); level && !s.airplayLevels.contains(to)) {
        level.key() = to;
        s.airplayLevels.insert(std::move(level));
    }
}

// A stereo pair paired again in the Home app comes back under a new ID. It is known by a speaker it shares with a saved
// pair, or by its name for one saved before its speakers were, and takes over what was saved under the old ID.
void followRepairedPairs(Settings& s, const DeviceCatalog& devices) {
    for (const auto& device : devices.airplay) {
        if (device.speakers.size() < 2) continue;
        const auto old = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) {
            if (o.backend != Backend::AirPlay || o.id.empty() || findDevice(devices, Backend::AirPlay, o.id)) return false;
            if (o.speakers.empty()) return o.name == device.name && !holds(device.speakers, o.id);
            return std::ranges::any_of(o.speakers, [&](const std::string& id) { return holds(device.speakers, id); });
        });
        if (old != s.outputs.end()) moveReceiver(s, std::string(old->id), device.id);
    }
}

// Speakers paired in the Home app are one receiver from then on (the owner, 2026-11-15): what was saved for each alone goes
// into the pair, which is in use and in the group where either was. A speaker still found alone keeps its own.
void mergePairedSpeakers(Settings& s, const DeviceCatalog& devices) {
    for (const auto& device : devices.airplay) {
        if (device.speakers.size() < 2) continue;
        for (const auto& speaker : device.speakers)
            if (speaker != device.id && !findDevice(devices, Backend::AirPlay, speaker) &&
                (findOutput(s, Backend::AirPlay, speaker) || holds(s.airplayGroup, speaker) ||
                 (s.backend == Backend::AirPlay && s.airplay.deviceId == speaker)))
                moveReceiver(s, speaker, device.id);
    }
}

// A player can answer under a new UDN, as the DMP-A6 did on 2026-11-19. The only one found unsaved with the name and model
// of a saved player that is gone is taken for it (the owner, 2026-11-19), one saved before models were kept by its name
// alone; what was saved for the old UDN, its use included, goes to the new one.
void followRenewedPlayers(Settings& s, const DeviceCatalog& devices) {
    for (auto& old : s.outputs) {
        if (old.backend != Backend::Network || old.id.empty() || findDevice(devices, Backend::Network, old.id)) continue;
        const auto fits = [&](const DeviceInfo& d) {
            return d.name == old.name && (old.model.empty() || d.model == old.model) && !findOutput(s, Backend::Network, d.id);
        };
        if (std::ranges::count_if(devices.network, fits) != 1) continue;
        const std::string renewed = std::ranges::find_if(devices.network, fits)->id;
        if (s.backend == Backend::Network && s.network.udn == old.id) s.network.udn = renewed;
        old.id = old.network.udn = renewed;
    }
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
        if (backend == Backend::AirPlay) {
            row.grouped = s.backend == Backend::AirPlay && holds(s.airplayGroup, id);
            const DeviceInfo* device = findDevice(devices, backend, id);
            row.pair = (device ? device->speakers.size() : saved ? saved->speakers.size() : 0) > 1;
        }
        rows.push_back(std::move(row));
    };
    for (const Backend backend :
         {Backend::Wasapi, Backend::Asio, Backend::Ks, Backend::Network, Backend::AirPlay, Backend::HQPlayer}) {
        if (backend == Backend::Wasapi) add(backend, {}, {}, true, {});
        for (const auto& device : devicesOf(devices, backend)) add(backend, device.id, device.name, true, device.unsupported);
        for (const auto& saved : s.outputs)
            if (saved.backend == backend && !saved.id.empty() && !findDevice(devices, backend, saved.id))
                add(backend, saved.id, {}, devices.revision == 0, {});
    }
    return rows;
}

bool needsFirewallRule(const OutputRow& row) {
    if (row.backend != Backend::HQPlayer) return isNetworkPlayer(row.backend);
    std::string host;
    uint16_t port = 0;
    return !hqplayer::splitAddress(row.id, host, port) || !hqplayer::isLocalHost(host);
}

void syncOutputs(Settings& s, const DeviceCatalog& devices) {
    if (devices.revision == 0) return;
    adoptSets(s, devices);
    followRepairedPairs(s, devices);
    mergePairedSpeakers(s, devices);
    followRenewedPlayers(s, devices);
    if (followsDefault(s) && !devices.defaultWasapi.empty() && s.followedDefault != devices.defaultWasapi) {
        keepParameters(s, Backend::Wasapi, s.followedDefault);
        loadParameters(s, Backend::Wasapi, devices.defaultWasapi, {});
        s.followedDefault = devices.defaultWasapi;
    }
    keepParameters(s, s.backend, deviceInUse(s));
    if (s.backend == Backend::Wasapi || !selectedId(s).empty()) savedOutput(s, s.backend, selectedId(s)).enabled = true;
    if (s.backend == Backend::AirPlay && !s.airplay.deviceId.empty() && !holds(s.airplayGroup, s.airplay.deviceId))
        s.airplayGroup.insert(s.airplayGroup.begin(), s.airplay.deviceId);
    for (auto& saved : s.outputs)
        if (const auto* device = findDevice(devices, saved.backend, saved.id)) {
            saved.name = device->name;
            if (!device->model.empty()) saved.model = device->model;
            if (!device->speakers.empty()) saved.speakers = device->speakers;
            const bool inUse = saved.backend == s.backend && saved.id == selectedId(s);
            if (!device->unsupported.empty() && !inUse) {
                saved.enabled = false;
                if (saved.backend == Backend::AirPlay) std::erase(s.airplayGroup, saved.id);
            }
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
        setOutputDsp(s, outputDsp(edited));
        if (backend == Backend::Asio)
            s.asio = edited.asio;
        else if (backend == Backend::Ks)
            s.ks = edited.ks;
        else if (backend == Backend::Network)
            s.network = edited.network;
        else if (backend == Backend::AirPlay)
            s.airplay = edited.airplay;
        else if (backend == Backend::HQPlayer)
            s.hqplayer = edited.hqplayer;
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
    if (backend == Backend::AirPlay && !holds(s.airplayGroup, id)) s.airplayGroup = {id};
    syncOutputs(s, devices);
}

void setInAirPlayGroup(Settings& s, const std::string& id, bool in, const DeviceCatalog& devices) {
    if (s.backend != Backend::AirPlay || id.empty() || holds(s.airplayGroup, id) == in) return;
    if (in) {
        s.airplayGroup.push_back(id);
        savedOutput(s, Backend::AirPlay, id).enabled = true;
    } else {
        if (s.airplayGroup.size() < 2) return;
        std::erase(s.airplayGroup, id);
        if (s.airplay.deviceId == id) {
            keepParameters(s, Backend::AirPlay, id);
            loadParameters(s, Backend::AirPlay, s.airplayGroup.front(), s.airplayGroup.front());
        }
    }
    syncOutputs(s, devices);
}

AirPlayChannels airplayChannels(const Settings& s, const std::string& id) {
    if (s.backend == Backend::AirPlay && s.airplay.deviceId == id) return s.airplay.channels;
    const SavedOutput* saved = findOutput(s, Backend::AirPlay, id);
    return saved ? saved->airplay.channels : AirPlayChannels::Stereo;
}

void setOutputEnabled(Settings& s, Backend backend, const std::string& id, bool enabled, const DeviceCatalog& devices) {
    if (s.backend == backend && selectedId(s) == id) return;
    savedOutput(s, backend, id).enabled = enabled;
    // The output menu lists only enabled receivers, so one turned off there could never be taken out of the group.
    if (!enabled && backend == Backend::AirPlay) std::erase(s.airplayGroup, id);
    syncOutputs(s, devices);
}

bool addHQPlayer(Settings& s, const std::string& address, const std::string& name, const DeviceCatalog& devices) {
    if (findOutput(s, Backend::HQPlayer, address)) return false;
    SavedOutput& added = savedOutput(s, Backend::HQPlayer, address);
    added.name = name;
    added.enabled = true;
    syncOutputs(s, devices);
    return true;
}

void renameOutput(Settings& s, Backend backend, const std::string& id, const std::string& alias, const DeviceCatalog& devices) {
    savedOutput(s, backend, id).alias = alias;
    syncOutputs(s, devices);
}

void deleteOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices) {
    if (id.empty()) return;
    std::erase_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    if (s.backend == backend && deviceInUse(s) == id) loadParameters(s, backend, id, selectedId(s));
    if (backend == Backend::AirPlay && (s.backend != backend || selectedId(s) != id)) std::erase(s.airplayGroup, id);
    syncOutputs(s, devices);
}

} // namespace camel::server
