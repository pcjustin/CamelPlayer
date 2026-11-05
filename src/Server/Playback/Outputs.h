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
    bool operator==(const OutputRow&) const = default;
};

// Every device found and every saved one, by backend; "Windows default device" first.
std::vector<OutputRow> outputRows(const Settings& s, const DeviceCatalog& devices);

// Brings `s` in line with `devices`: gives an earlier version's sets to their devices, moves to the default device's
// set when the followed default changed, keeps the parameters in use and the devices' names, enables the output in use
// and disables any other that cannot be used. Does nothing before the first device scan.
void syncOutputs(Settings& s, const DeviceCatalog& devices);

void selectOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices);
// The output in use stays enabled.
void setOutputEnabled(Settings& s, Backend backend, const std::string& id, bool enabled, const DeviceCatalog& devices);
void renameOutput(Settings& s, Backend backend, const std::string& id, const std::string& alias, const DeviceCatalog& devices);
// Clears its parameters, name and enabled state; found again, it starts from the defaults. "Windows default device" is
// not a device and stays.
void deleteOutput(Settings& s, Backend backend, const std::string& id, const DeviceCatalog& devices);

} // namespace camel::server
