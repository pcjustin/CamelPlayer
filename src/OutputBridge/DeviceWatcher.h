// Windows default render endpoint changes, for WASAPI "follow Windows default" (docs/Plan.md §2).
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

} // namespace camel::audio
