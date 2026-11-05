// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The inbound firewall rule network players need to reach CamelPlayer (docs/Plan.md §5.10): for this program, on private
// networks only, added once through UAC after CamelPlayer has said why.
#pragma once

#include <string>

namespace camel::server {

// Needs COM initialized on the calling thread.
bool firewallRuleExists();
// What netsh is given for `program`.
std::wstring firewallRuleArguments(const std::wstring& program);
// Asks for elevation and waits. False when it was declined or failed.
bool addFirewallRule();

} // namespace camel::server
