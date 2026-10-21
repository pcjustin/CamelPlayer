// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// WASAPI exclusive mode, event or polling driven (docs/Plan.md §5.5, §16.2). Shared mode arrives in M1.
#pragma once
#include "OutputBridge/IOutput.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

std::unique_ptr<IOutput> createWasapiOutput(const WasapiSettings& s);

} // namespace camel::audio
