// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Firewall.h"

#include <windows.h>
// windows.h comes first
#include <netfw.h>
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

// Each copy of CamelPlayer has its own rule under one name, so every rule of that name is looked at.
bool firewallRuleExists() {
    const auto policy = wil::CoCreateInstanceNoThrow<NetFwPolicy2, INetFwPolicy2>();
    wil::com_ptr<INetFwRules> rules;
    if (!policy || FAILED(policy->get_Rules(&rules))) return false;
    wil::com_ptr<IUnknown> enumerable;
    if (FAILED(rules->get__NewEnum(&enumerable))) return false;
    const auto items = enumerable.try_query<IEnumVARIANT>();
    if (!items) return false;
    const std::wstring program = programPath();
    for (;;) {
        wil::unique_variant item;
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || fetched == 0) return false;
        if (item.vt != VT_DISPATCH || !item.pdispVal) continue;
        wil::com_ptr<INetFwRule> rule;
        if (FAILED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) continue;
        wil::unique_bstr name, application;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(rule->get_Name(&name)) || !name || wcscmp(name.get(), kRuleName) != 0) continue;
        if (SUCCEEDED(rule->get_Enabled(&enabled)) && enabled != VARIANT_FALSE &&
            SUCCEEDED(rule->get_ApplicationName(&application)) && application &&
            _wcsicmp(application.get(), program.c_str()) == 0)
            return true;
    }
}

std::wstring firewallRuleArguments(const std::wstring& program) {
    return std::wstring(L"advfirewall firewall add rule name=\"") + kRuleName + L"\" dir=in action=allow program=\"" + program +
           L"\" profile=private enable=yes";
}

bool addFirewallRule() {
    const std::wstring arguments = firewallRuleArguments(programPath());
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = L"netsh.exe";
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) return false;
    const wil::unique_handle process(execute.hProcess);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 1;
    return GetExitCodeProcess(process.get(), &code) && code == 0;
}

} // namespace camel::server
