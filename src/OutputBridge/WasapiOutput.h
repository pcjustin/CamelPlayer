// WASAPI exclusive mode, event or polling driven (docs/Plan.md §5.5, §16.2). Shared mode arrives in M1.
#pragma once
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createWasapiOutput(const WasapiSettings& s);

} // namespace camel::audio
