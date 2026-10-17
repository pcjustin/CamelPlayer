// Kernel Streaming: WaveRT event / WaveRT polling / standard streaming (docs/Plan.md §5.7, §15.5).
#pragma once
#include "audio/output/IOutput.h"
#include "settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createKsOutput(const KsSettings& s);

} // namespace camel::audio
