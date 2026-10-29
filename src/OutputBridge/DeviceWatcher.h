// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Windows default render endpoint changes, for WASAPI "follow Windows default" (docs/Plan.md §2), and audio devices
// appearing or going away, for the device lists.
#pragma once
#include <functional>
#include <memory>

namespace camel::audio {

// Reports changes until destroyed; no callback runs after the destructor returns.
class DeviceWatcher {
public:
    virtual ~DeviceWatcher() = default;
};

// `onDefaultChanged` runs on a system thread whenever the default console render endpoint changes; keep it short.
using DeviceWatcherFactory = std::function<std::unique_ptr<DeviceWatcher>(std::function<void()> onDefaultChanged)>;

// Needs COM initialized on the calling thread. Throws Error when the endpoint enumerator is unavailable.
std::unique_ptr<DeviceWatcher> watchDefaultRenderDevice(std::function<void()> onDefaultChanged);

// `onChange` runs on a system thread whenever an audio endpoint is added, removed or changes state, as when a DAC is
// switched on or off. Same requirements as above.
std::unique_ptr<DeviceWatcher> watchAudioDevices(std::function<void()> onChange);

} // namespace camel::audio
