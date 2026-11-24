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

// The entry's ID, which every kind of output's settings in it hold as reading it back gives them.
void setId(SavedOutput& o, const std::string& id) {
    o.id = o.wasapi.deviceId = o.asio.driverClsid = o.ks.deviceId = o.network.udn = o.airplay.deviceId = o.hqplayer.address = id;
}

SavedOutput& savedOutput(Settings& s, Backend backend, const std::string& id) {
    const auto found = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == backend && o.id == id; });
    if (found != s.outputs.end()) return *found;
    SavedOutput added = newOutput(backend);
    // A new output starts at the level playing now rather than at full volume.
    added.volumeDb = s.volume.db;
    added.volumeMuted = s.volume.muted;
    setId(added, id);
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
    saved.volumeDb = s.volume.db;
    saved.volumeMuted = s.volume.muted;
    saved.dsp = outputDsp(s);
    saved.zone = zoneChoices(s);
    if (backend == Backend::Asio)
        saved.asio = s.asio;
    else if (backend == Backend::Ks)
        saved.ks = s.ks;
    else if (backend == Backend::Network)
        saved.network = s.network;
    else if (backend == Backend::AirPlay) {
        saved.airplay = s.airplay;
        saved.airplayGroup = s.airplayGroup.size() > 1 ? s.airplayGroup : std::vector<std::string>();
    } else if (backend == Backend::HQPlayer)
        saved.hqplayer = s.hqplayer;
    else
        saved.wasapi = s.wasapi;
    setId(saved, id);
}

// Puts `device`'s parameters, or the defaults for a device without any, in Settings' fields for the output `selected`.
void loadParameters(Settings& s, Backend backend, const std::string& device, std::string selected) {
    const SavedOutput* saved = device.empty() ? nullptr : findOutput(s, backend, device);
    const SavedOutput defaults = newOutput(backend);
    const SavedOutput& source = saved ? *saved : defaults;
    s.backend = backend;
    s.volume.mode = source.volumeMode;
    if (isNetworkPlayer(backend) && s.volume.mode == VolumeMode::Software) s.volume.mode = VolumeMode::Player;
    if (saved) {
        s.volume.db = saved->volumeDb;
        s.volume.muted = saved->volumeMuted;
    }
    setOutputDsp(s, source.dsp);
    setZoneChoices(s, source.zone);
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
        // A receiver of the group last played, without one of its own, brings that group back.
        if (!source.airplayGroup.empty()) s.airplayGroup = source.airplayGroup;
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
void moveReceiver(Settings& s, const std::string& from, const std::string& to, std::vector<OutputMove>& moves) {
    moves.push_back({Backend::AirPlay, from, to});
    const auto saved = [&](const std::string& id) {
        return std::ranges::find_if(s.outputs, [&](const SavedOutput& o) { return o.backend == Backend::AirPlay && o.id == id; });
    };
    if (const auto old = saved(from); old != s.outputs.end()) {
        if (const auto kept = saved(to); kept != s.outputs.end()) {
            kept->enabled = kept->enabled || old->enabled;
            s.outputs.erase(old);
        } else {
            setId(*old, to);
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
// pair, or by its name for one saved before its speakers were, when no other saved receiver has that name (MZ-R14), and
// takes over what was saved under the old ID.
void followRepairedPairs(Settings& s, const DeviceCatalog& devices, std::vector<OutputMove>& moves) {
    for (const auto& device : devices.airplay) {
        if (device.speakers.size() < 2) continue;
        // Its own speakers, which go into it, do not count.
        const auto named = [&](const std::string& name) {
            return std::ranges::count_if(s.outputs, [&](const SavedOutput& o) {
                return o.backend == Backend::AirPlay && o.name == name && !holds(device.speakers, o.id);
            });
        };
        const auto old = std::ranges::find_if(s.outputs, [&](const SavedOutput& o) {
            if (o.backend != Backend::AirPlay || o.id.empty() || findDevice(devices, Backend::AirPlay, o.id)) return false;
            if (o.speakers.empty()) return o.name == device.name && !holds(device.speakers, o.id) && named(o.name) == 1;
            return std::ranges::any_of(o.speakers, [&](const std::string& id) { return holds(device.speakers, id); });
        });
        if (old != s.outputs.end()) moveReceiver(s, std::string(old->id), device.id, moves);
    }
}

// Speakers paired in the Home app are one receiver from then on (the owner, 2026-11-15): what was saved for each alone goes
// into the pair, which is in use and in the group where either was. A speaker still found alone keeps its own.
void mergePairedSpeakers(Settings& s, const DeviceCatalog& devices, std::vector<OutputMove>& moves) {
    for (const auto& device : devices.airplay) {
        if (device.speakers.size() < 2) continue;
        for (const auto& speaker : device.speakers)
            if (speaker != device.id && !findDevice(devices, Backend::AirPlay, speaker) &&
                (findOutput(s, Backend::AirPlay, speaker) || holds(s.airplayGroup, speaker) ||
                 (s.backend == Backend::AirPlay && s.airplay.deviceId == speaker)))
                moveReceiver(s, speaker, device.id, moves);
    }
}

// A player can answer under a new UDN, as the DMP-A6 did on 2026-11-19. A saved player that is gone is taken for the
// only one found unsaved with its MAC address (the owner, 2026-11-22); when either MAC is unknown, for the only one with
// its name and model, as long as no other saved player shares them (the owner, 2026-11-19 and 2026-11-22), one saved
// before models were kept by its name alone. What was saved for the old UDN, its use included, goes to the new one.
void followRenewedPlayers(Settings& s, const DeviceCatalog& devices, std::vector<OutputMove>& moves) {
    for (auto& old : s.outputs) {
        if (old.backend != Backend::Network || old.id.empty() || findDevice(devices, Backend::Network, old.id)) continue;
        const auto unsaved = [&](const DeviceInfo& d) { return !findOutput(s, Backend::Network, d.id); };
        const auto sameMac = [&](const DeviceInfo& d) { return !old.mac.empty() && d.mac == old.mac && unsaved(d); };
        const auto sameName = [&](const std::string& name, const std::string& model) {
            return name == old.name && (old.model.empty() || model == old.model);
        };
        const auto fits = [&](const DeviceInfo& d) {
            return sameName(d.name, d.model) && (old.mac.empty() || d.mac.empty()) && unsaved(d);
        };
        const bool shared = std::ranges::any_of(s.outputs, [&](const SavedOutput& o) {
            return &o != &old && o.backend == Backend::Network && sameName(o.name, o.model);
        });
        const DeviceInfo* renewed = nullptr;
        if (std::ranges::count_if(devices.network, sameMac) == 1)
            renewed = &*std::ranges::find_if(devices.network, sameMac);
        else if (!shared && std::ranges::count_if(devices.network, fits) == 1)
            renewed = &*std::ranges::find_if(devices.network, fits);
        if (!renewed) continue;
        if (s.backend == Backend::Network && s.network.udn == old.id) s.network.udn = renewed->id;
        moves.push_back({Backend::Network, old.id, renewed->id});
        setId(old, renewed->id);
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

const std::string& outputInUse(const Settings& s) {
    return selectedId(s);
}

bool needsFirewallRule(const OutputRow& row) {
    if (row.backend != Backend::HQPlayer) return isNetworkPlayer(row.backend);
    std::string host;
    uint16_t port = 0;
    return !hqplayer::splitAddress(row.id, host, port) || !hqplayer::isLocalHost(host);
}

std::vector<OutputMove> syncOutputs(Settings& s, const DeviceCatalog& devices) {
    std::vector<OutputMove> moves;
    if (devices.revision == 0) return moves;
    // In the order the scan found them, which the output menu and the zone picker show before the next scan too.
    const auto found = [&](const SavedOutput& o) {
        const auto& list = devicesOf(devices, o.backend);
        const auto at = std::ranges::find(list, o.id, &DeviceInfo::id);
        return std::pair(int(o.backend), at == list.end() ? list.size() : size_t(at - list.begin()));
    };
    std::ranges::stable_sort(s.outputs, {}, found);
    adoptSets(s, devices);
    followRepairedPairs(s, devices, moves);
    mergePairedSpeakers(s, devices, moves);
    followRenewedPlayers(s, devices, moves);
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
            if (!device->mac.empty()) saved.mac = device->mac;
            if (!device->speakers.empty()) saved.speakers = device->speakers;
            const bool inUse = saved.backend == s.backend && saved.id == selectedId(s);
            if (!device->unsupported.empty() && !inUse) {
                saved.enabled = false;
                if (saved.backend == Backend::AirPlay) std::erase(s.airplayGroup, saved.id);
            }
        }
    return moves;
}

bool nameTaken(const std::vector<OutputRow>& rows, const OutputRow& row) {
    return std::ranges::any_of(rows, [&](const OutputRow& other) {
        return other.enabled && other.backend == row.backend && other.id != row.id && other.name == row.name;
    });
}

std::string freeName(const std::vector<OutputRow>& rows, const OutputRow& row) {
    for (int n = 2;; ++n) {
        OutputRow renamed = row;
        renamed.name = row.name + " (" + std::to_string(n) + ")";
        if (!nameTaken(rows, renamed)) return renamed.name;
    }
}

Settings withOutputParameters(Settings s, Backend backend, const std::string& id, const DeviceCatalog& devices) {
    if (s.backend == backend && selectedId(s) == id) return s;
    const std::string device = parameterDevice(devices, backend, id);
    loadParameters(s, backend, device, id);
    if (backend == Backend::AirPlay) {
        const SavedOutput* saved = findOutput(s, backend, id);
        s.airplayGroup = saved && !saved->airplayGroup.empty() ? saved->airplayGroup : std::vector{id};
    }
    return s;
}

void leaveAirPlayGroup(Settings& s, const std::string& owner, const std::string& id) {
    if (s.backend == Backend::AirPlay && s.airplay.deviceId == owner) return;
    SavedOutput& saved = savedOutput(s, Backend::AirPlay, owner);
    std::erase(saved.airplayGroup, id);
    if (saved.airplayGroup.size() < 2) saved.airplayGroup.clear();
}

void keepQueueChoices(Settings& s, Backend backend, const std::string& id, bool shuffle, RepeatMode repeat,
                      const DeviceCatalog& devices) {
    if (s.backend == backend && selectedId(s) == id) {
        s.shuffle = shuffle;
        s.repeat = repeat;
        return;
    }
    const std::string device = parameterDevice(devices, backend, id);
    if (device.empty()) return;
    SavedOutput& saved = savedOutput(s, backend, device);
    saved.zone.shuffle = shuffle;
    saved.zone.repeat = repeat;
}

void keepOutputParameters(Settings& s, const Settings& edited, Backend backend, const std::string& id,
                          const DeviceCatalog& devices) {
    const bool inUse = s.backend == backend && selectedId(s) == id;
    if (inUse) {
        setOutputDsp(s, outputDsp(edited));
        s.crossfade = edited.crossfade;
        s.autoplay = edited.autoplay;
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
    kept.volume.db = saved ? saved->volumeDb : s.volume.db;
    kept.volume.muted = saved ? saved->volumeMuted : s.volume.muted;
    // The queue's, which may have changed while the dialog was open.
    kept.shuffle = saved ? saved->zone.shuffle : false;
    kept.repeat = saved ? saved->zone.repeat : RepeatMode::Off;
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
            // The group goes on under the first receiver left with its DSP, choices and volume; the one that left keeps
            // them as its own, alone (MZ-R08).
            keepParameters(s, Backend::AirPlay, id);
            savedOutput(s, Backend::AirPlay, id).airplayGroup.clear();
            const std::string next = s.airplayGroup.front();
            const SavedOutput* own = findOutput(s, Backend::AirPlay, next);
            s.airplay = own ? own->airplay : AirPlaySettings{};
            s.airplay.deviceId = next;
        }
    }
    syncOutputs(s, devices);
}

std::string passAirPlayGroup(Settings& s, const std::string& owner) {
    const SavedOutput* found = findOutput(s, Backend::AirPlay, owner);
    if (!found) return {};
    auto rest = found->airplayGroup;
    std::erase(rest, owner);
    if (rest.empty()) return {};
    SavedOutput& to = savedOutput(s, Backend::AirPlay, rest.front());
    const SavedOutput& from = *findOutput(s, Backend::AirPlay, owner);
    to.dsp = from.dsp;
    to.zone = from.zone;
    to.volumeMode = from.volumeMode;
    to.volumeDb = from.volumeDb;
    to.volumeMuted = from.volumeMuted;
    to.enabled = true;
    to.airplayGroup = rest.size() > 1 ? rest : std::vector<std::string>();
    savedOutput(s, Backend::AirPlay, owner).airplayGroup.clear();
    return rest.front();
}

bool splitZoneId(const std::string& zone, Backend& backend, std::string& id) {
    const auto colon = zone.find(':');
    if (colon == std::string::npos || !backendFromId(std::string_view(zone).substr(0, colon), backend)) return false;
    id = zone.substr(colon + 1);
    return true;
}

std::vector<std::string> localGroupOf(const Settings& s, const std::string& owner) {
    Backend backend{};
    std::string id;
    if (!splitZoneId(owner, backend, id)) return {owner};
    const SavedOutput* saved = findOutput(s, backend, id);
    return saved && saved->localGroup.size() > 1 ? saved->localGroup : std::vector{owner};
}

std::string localGroupClock(const Settings& s, const std::string& owner) {
    const auto group = localGroupOf(s, owner);
    Backend backend{};
    std::string id;
    const SavedOutput* saved = splitZoneId(owner, backend, id) ? findOutput(s, backend, id) : nullptr;
    return saved && holds(group, saved->groupClock) ? saved->groupClock : group.front();
}

std::vector<std::pair<std::string, std::string>>
groupLocal(Settings& s, const std::string& owner, const std::vector<std::string>& members, const DeviceCatalog& devices) {
    std::vector<std::pair<std::string, std::string>> moves;
    Backend ownerBackend{};
    std::string ownerId;
    if (!splitZoneId(owner, ownerBackend, ownerId)) return moves;
    const auto entry = [&](const std::string& zone) -> SavedOutput& {
        Backend backend{};
        std::string id;
        splitZoneId(zone, backend, id);
        return savedOutput(s, backend, id);
    };
    std::vector<std::string> group{owner};
    for (const auto& zone : members) {
        Backend backend{};
        std::string id;
        if (splitZoneId(zone, backend, id) && isLocalOutput(backend) && !holds(group, zone)) group.push_back(zone);
    }
    const auto before = localGroupOf(s, owner);
    for (size_t i = 1; i < group.size(); ++i) {
        const std::string& zone = group[i];
        if (holds(before, zone)) continue;
        for (const auto& saved : std::vector(s.outputs)) {
            const std::string key = zoneId(saved.backend, saved.id);
            if (key == owner || !holds(saved.localGroup, zone)) continue;
            if (key == zone) {
                auto rest = saved.localGroup;
                std::erase(rest, zone);
                if (!rest.empty()) {
                    SavedOutput& next = entry(rest.front());
                    next.groupClock = holds(rest, saved.groupClock) && rest.size() > 1 ? saved.groupClock : std::string();
                    next.localGroup = rest.size() > 1 ? rest : std::vector<std::string>();
                    moves.emplace_back(zone, rest.front());
                }
            } else {
                SavedOutput& from = entry(key);
                std::erase(from.localGroup, zone);
                if (from.groupClock == zone || from.localGroup.size() < 2) from.groupClock.clear();
                if (from.localGroup.size() < 2) from.localGroup.clear();
            }
        }
        SavedOutput& joined = entry(zone);
        joined.localGroup.clear();
        joined.groupClock.clear();
    }
    for (const auto& zone : group) entry(zone).enabled = true;
    SavedOutput& own = entry(owner);
    own.localGroup = group.size() > 1 ? group : std::vector<std::string>();
    if (!holds(own.localGroup, own.groupClock)) own.groupClock.clear();
    const std::string inUse = zoneId(s.backend, selectedId(s));
    if (std::find(group.begin() + 1, group.end(), inUse) != group.end())
        selectOutput(s, ownerBackend, ownerId, devices);
    else
        syncOutputs(s, devices);
    return moves;
}

void setLocalGroupClock(Settings& s, const std::string& owner, const std::string& clock) {
    Backend backend{};
    std::string id;
    if (!splitZoneId(owner, backend, id)) return;
    SavedOutput& own = savedOutput(s, backend, id);
    if (holds(own.localGroup, clock)) own.groupClock = clock == own.localGroup.front() ? std::string() : clock;
}

void setGroupDelay(Settings& s, Backend backend, const std::string& id, int32_t ms) {
    savedOutput(s, backend, id).groupDelayMs = std::clamp(ms, -SavedOutput::kMaxGroupDelayMs, SavedOutput::kMaxGroupDelayMs);
}

std::vector<OutputMove> groupAirPlay(Settings& s, const std::string& first, const std::vector<std::string>& members,
                                     const DeviceCatalog& devices) {
    std::vector<OutputMove> moves;
    const auto inUse = [&](const std::string& id) { return s.backend == Backend::AirPlay && s.airplay.deviceId == id; };
    const auto groupOf = [&](const std::string& id) {
        if (inUse(id)) return s.airplayGroup;
        const SavedOutput* saved = findOutput(s, Backend::AirPlay, id);
        return saved ? saved->airplayGroup : std::vector<std::string>();
    };
    std::vector<std::string> group{first};
    for (const auto& id : members)
        if (!id.empty() && !holds(group, id)) group.push_back(id);
    const auto before = groupOf(first);
    bool joinedInUse = false;
    for (size_t i = 1; i < group.size(); ++i) {
        const std::string& id = group[i];
        if (holds(before, id)) continue;
        if (inUse(id) && s.airplayGroup.size() < 2)
            joinedInUse = true;
        else if (s.backend == Backend::AirPlay && holds(s.airplayGroup, id))
            setInAirPlayGroup(s, id, false, devices);
        for (const auto& saved : std::vector(s.outputs))
            if (saved.backend == Backend::AirPlay && saved.id != first && holds(saved.airplayGroup, id)) {
                if (saved.id != id)
                    leaveAirPlayGroup(s, saved.id, id);
                else if (const auto next = inUse(id) ? std::string() : passAirPlayGroup(s, id); !next.empty())
                    moves.push_back({Backend::AirPlay, id, next});
            }
    }
    for (const auto& id : group) savedOutput(s, Backend::AirPlay, id).enabled = true;
    if (joinedInUse) {
        selectOutput(s, Backend::AirPlay, first, devices);
        s.airplayGroup = group;
    } else if (inUse(first)) {
        s.airplayGroup = group;
    } else {
        savedOutput(s, Backend::AirPlay, first).airplayGroup = group.size() > 1 ? group : std::vector<std::string>();
    }
    syncOutputs(s, devices);
    return moves;
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
