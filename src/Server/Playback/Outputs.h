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

// Brings `s` in line with `devices`: gives an earlier version's sets to their devices, moves to the default device's
// set when the followed default changed, keeps the parameters in use and the devices' names, enables the output in use
// and disables any other that cannot be used. Does nothing before the first device scan.
void syncOutputs(Settings& s, const DeviceCatalog& devices);

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
