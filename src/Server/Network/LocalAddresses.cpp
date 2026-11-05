// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/LocalAddresses.h"

#include <ws2tcpip.h>
// ws2tcpip.h comes first
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace camel::server {

std::vector<in_addr> localAddresses() {
    std::vector<in_addr> addresses;
    ULONG size = 16 * 1024;
    std::vector<char> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return addresses;
    for (auto* adapter = list; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
            addresses.push_back(reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr);
    }
    return addresses;
}

} // namespace camel::server
