// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The output devices of Settings › Audio and the output menu (docs/Plan.md §6, §8.1). Each saved device keeps its own
// parameters; the output in use keeps them in Settings' backend fields, which the Engine reads.
#pragma once

#include <string>
#include <vector>

#include "Server/Api/ServerApi.h"

namespace camel::server {

struct OutputRow {
    Backend backend = Backend::Wasapi;
    std::string id;          // empty on WASAPI is "Windows default device"
    std::string name;        // the user's name or the device's; empty for "Windows default device" unrenamed
    std::string alias;       // the user's name alone
    bool enabled = false;    // listed in the output menu
    bool connected = true;   // found by the last device scan
    bool selected = false;   // the output in use
    bool saved = false;      // has anything to delete
    std::string unsupported; // why an AirPlay receiver cannot be enabled; empty when it can
    bool grouped = false;    // an AirPlay receiver of the group in use, the one in use among them
    bool pair = false;       // an AirPlay stereo pair, which always plays both channels
    bool operator==(const OutputRow&) const = default;
};

// Every device found and every saved one, by backend; "Windows default device" first.
std::vector<OutputRow> outputRows(const Settings& s, const DeviceCatalog& devices);

// The output in use's ID, as its row has it.
const std::string& outputInUse(const Settings& s);

// Fetches the stream from another computer, through the Windows Firewall rule.
bool needsFirewallRule(const OutputRow& row);

// An output that came back under another ID, as a player under a new UDN or speakers paired again, which its zone follows.
struct OutputMove {
    Backend backend = Backend::Wasapi;
    std::string from, to;
    bool operator==(const OutputMove&) const = default;
};

// Brings `s` in line with `devices`: gives an earlier version's sets to their devices, moves to the default device's
// set when the followed default changed, keeps the parameters in use and the devices' names, models and MACs, enables
// the output in use and disables any other that cannot be used. Does nothing before the first device scan. Returns the
// outputs that came back under another ID.
std::vector<OutputMove> syncOutputs(Settings& s, const DeviceCatalog& devices);

// Whether another enabled output of `row`'s backend shows the same name, which no two may (docs/Plan.md §5.12); and the
// name offered instead, "DMP-A6 (2)" and on.
bool nameTaken(const std::vector<OutputRow>& rows, const OutputRow& row);
std::string freeName(const std::vector<OutputRow>& rows, const OutputRow& row);

// Choosing a receiver of the AirPlay group last played brings the whole group back; any other starts a group of one.
void selectOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices);
// While AirPlay is in use: adds a receiver to the group, or takes one out, the last one staying. When the one in use
// leaves, the next takes its place in Settings' AirPlay fields.
void setInAirPlayGroup(Settings& s, const std::string& id, bool in, const DeviceCatalog& devices);
// Takes a receiver out of the group of `owner`, an AirPlay zone's receiver not in use (MZ-R05); the one in use's group
// changes through setInAirPlayGroup.
void leaveAirPlayGroup(Settings& s, const std::string& owner, const std::string& id);
// The group of `owner`, an AirPlay receiver not in use, goes on under the first receiver left, which takes the owner's
// DSP, zone choices and volume; the owner is left alone (MZ-R08). Returns that receiver; empty when the owner was alone.
std::string passAirPlayGroup(Settings& s, const std::string& owner);
// The AirPlay zone of `first` plays as one group with `members` (MZ-R24), each taken out of the group it was in; a
// receiver the group held and does not now is alone again, and no members ungroup it. A group a member was formed with
// goes on under the next of its receivers (MZ-R08), which the moves name for its zone to follow. The receiver in use,
// alone, joining makes `first` the one in use.
std::vector<OutputMove> groupAirPlay(Settings& s, const std::string& first, const std::vector<std::string>& members,
                                     const DeviceCatalog& devices);
// A zone's backend and output ID from its key ("asio:<id>"); false for a key that names none.
bool splitZoneId(const std::string& zone, Backend& backend, std::string& id);
// The group of this computer's outputs the zone `owner` plays as, itself first; alone, itself (MZ-R25).
std::vector<std::string> localGroupOf(const Settings& s, const std::string& owner);
// The member of `owner`'s group the others keep to: the one saved as its clock while it is a member, else the first.
std::string localGroupClock(const Settings& s, const std::string& owner);
// The zone `owner`, of this computer's outputs, plays with `members` from now on, each taken out of the group it was
// in; none ungroups it. A group a member was formed with goes on under its next member, which the moves name, as zone
// keys, for its zone to follow. A member in use makes `owner` the output in use.
std::vector<std::pair<std::string, std::string>>
groupLocal(Settings& s, const std::string& owner, const std::vector<std::string>& members, const DeviceCatalog& devices);
void setLocalGroupClock(Settings& s, const std::string& owner, const std::string& clock);
// Milliseconds the output plays after its group's clock.
void setGroupDelay(Settings& s, Backend backend, const std::string& id, int32_t ms);
// A group receiver's channels: the one in use from Settings' fields, the others from their saved outputs.
AirPlayChannels airplayChannels(const Settings& s, const std::string& id);
// `s` as it would be with this output in use, for showing its parameters without leaving the one in use.
Settings withOutputParameters(Settings s, Backend backend, const std::string& id, const DeviceCatalog& devices);
// The shuffle and repeat of this output's zone, which only the Server sets: in Settings' fields for the output in use.
void keepQueueChoices(Settings& s, Backend backend, const std::string& id, bool shuffle, RepeatMode repeat,
                      const DeviceCatalog& devices);
// Takes this output's parameters from `edited`, which came from withOutputParameters. An output not in use only keeps them.
void keepOutputParameters(Settings& s, const Settings& edited, Backend backend, const std::string& id,
                          const DeviceCatalog& devices);
// The output in use stays enabled.
void setOutputEnabled(Settings& s, Backend backend, const std::string& id, bool enabled, const DeviceCatalog& devices);
// An HQPlayer by its address, "host:port", enabled; false when it is saved already.
bool addHQPlayer(Settings& s, const std::string& address, const std::string& name, const DeviceCatalog& devices);
void renameOutput(Settings& s, Backend backend, const std::string& id, const std::string& alias, const DeviceCatalog& devices);
// Clears its parameters, name and enabled state; found again, it starts from the defaults. "Windows default device" is
// not a device and stays.
void deleteOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices);

} // namespace camel::server
