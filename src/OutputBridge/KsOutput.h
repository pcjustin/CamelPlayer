// Kernel Streaming: WaveRT event / WaveRT polling / standard streaming (docs/Plan.md §5.7, §15.5).
#pragma once
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createKsOutput(const KsSettings& s);

} // namespace camel::audio
