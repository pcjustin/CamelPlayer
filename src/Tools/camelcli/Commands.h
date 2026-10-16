#pragma once
#include <optional>
#include <string>
#include <vector>

namespace camelcli {

using Args = std::vector<std::wstring>;

// Returns the value after `--name`, if present.
std::optional<std::wstring> option(const Args& args, const wchar_t* name);
bool flag(const Args& args, const wchar_t* name);
std::vector<std::wstring> positional(const Args& args);

int cmdDecode(const Args& args);
int cmdListDevices(const Args& args);
int cmdPlay(const Args& args);
int cmdCapture(const Args& args);
int cmdGenTestAudio(const Args& args);
int cmdBenchOne(const Args& args);
int cmdBenchDecoders(const Args& args);

} // namespace camelcli
