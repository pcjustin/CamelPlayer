// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Firewall.h"

#include <windows.h>
// windows.h comes first
#include <netfw.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <vector>

#pragma comment(lib, "shell32.lib")

namespace camel::server {

namespace {

constexpr wchar_t kRuleName[] = L"CamelPlayer network players";

std::wstring programPath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    return {path.data(), length};
}

} // namespace

bool firewallRuleExists() {
    return firewallRuleFor(programPath()).has_value();
}

// Each copy of CamelPlayer has its own rule under one name, so every rule of that name is looked at.
std::optional<FirewallRuleState> firewallRuleFor(const std::wstring& program) {
    const auto policy = wil::CoCreateInstanceNoThrow<NetFwPolicy2, INetFwPolicy2>();
    wil::com_ptr<INetFwRules> rules;
    if (!policy || FAILED(policy->get_Rules(&rules))) return std::nullopt;
    wil::com_ptr<IUnknown> enumerable;
    if (FAILED(rules->get__NewEnum(&enumerable))) return std::nullopt;
    const auto items = enumerable.try_query<IEnumVARIANT>();
    if (!items) return std::nullopt;
    for (;;) {
        wil::unique_variant item;
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || fetched == 0) return std::nullopt;
        if (item.vt != VT_DISPATCH || !item.pdispVal) continue;
        wil::com_ptr<INetFwRule> rule;
        if (FAILED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) continue;
        wil::unique_bstr name, application;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(rule->get_Name(&name)) || !name || wcscmp(name.get(), kRuleName) != 0) continue;
        if (FAILED(rule->get_Enabled(&enabled)) || enabled == VARIANT_FALSE || FAILED(rule->get_ApplicationName(&application)) ||
            !application || _wcsicmp(application.get(), program.c_str()) != 0)
            continue;
        FirewallRuleState state;
        rule->get_Profiles(&state.profiles);
        if (wil::unique_bstr remote; SUCCEEDED(rule->get_RemoteAddresses(&remote)) && remote)
            state.remoteAddresses = remote.get();
        return state;
    }
}

bool publicNetworkConnected() {
    const auto manager = wil::CoCreateInstanceNoThrow<NetworkListManager, INetworkListManager>();
    wil::com_ptr<IEnumNetworks> networks;
    if (!manager || FAILED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks))) return false;
    for (;;) {
        wil::com_ptr<INetwork> network;
        ULONG fetched = 0;
        if (networks->Next(1, &network, &fetched) != S_OK || fetched == 0) return false;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PRIVATE;
        if (SUCCEEDED(network->GetCategory(&category)) && category == NLM_NETWORK_CATEGORY_PUBLIC) return true;
    }
}

std::wstring firewallRuleArguments(const std::wstring& program) {
    return std::wstring(L"advfirewall firewall add rule name=\"") + kRuleName + L"\" dir=in action=allow program=\"" + program +
           L"\" profile=private enable=yes";
}

std::wstring firewallTool() {
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::wstring(system, length) + L"\\netsh.exe";
}

bool addFirewallRule() {
    const std::wstring tool = firewallTool();
    if (tool.empty()) return false;
    const std::wstring arguments = firewallRuleArguments(programPath());
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = tool.c_str();
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) return false;
    const wil::unique_handle process(execute.hProcess);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 1;
    return GetExitCodeProcess(process.get(), &code) && code == 0;
}

} // namespace camel::server
