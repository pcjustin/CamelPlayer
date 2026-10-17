// ASIO (PCM) with a dedicated STA control thread (docs/Plan.md §5.6, §15.3). Native DSD arrives in M2.
#pragma once
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createAsioOutput(const AsioSettings& s);

// Opens the driver's control panel; false if `out` is not an ASIO output or the driver refused.
bool showAsioControlPanel(IOutput* out);

} // namespace camel::audio
