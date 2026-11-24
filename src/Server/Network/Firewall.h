// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The inbound firewall rule network players need to reach CamelPlayer (docs/Plan.md §5.10): for this program, on private
// networks only, added once through UAC after CamelPlayer has said why.
#pragma once

#include <optional>
#include <string>

namespace camel::server {

// The rule as Windows keeps it, from NET_FW_PROFILE_TYPE2 and the rule's remote addresses ("*" for any).
struct FirewallRuleState {
    long profiles = 0;
    std::wstring remoteAddresses;
};

// Needs COM initialized on the calling thread.
bool firewallRuleExists();
// The enabled rule of this name for `program`; nothing when there is none. Needs COM.
std::optional<FirewallRuleState> firewallRuleFor(const std::wstring& program);
// Whether a network Windows calls public is connected, where the rule, kept for private networks, lets nothing in
// (docs/Plan.md §17.8). Needs COM.
bool publicNetworkConnected();
// What netsh is given for `program`.
std::wstring firewallRuleArguments(const std::wstring& program);
// The netsh that is started.
std::wstring firewallTool();
// Asks for elevation and waits. False when it was declined or failed.
bool addFirewallRule();

} // namespace camel::server
