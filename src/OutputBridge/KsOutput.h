// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Kernel Streaming: WaveRT event / WaveRT polling / standard streaming (docs/Plan.md §5.7, §15.5).
#pragma once
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createKsOutput(const KsSettings& s);

} // namespace camel::audio
