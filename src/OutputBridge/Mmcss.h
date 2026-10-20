// Joins the calling output thread to the configured MMCSS task and priority (docs/Plan.md §5.5, §6) until destroyed.
#pragma once
#include <windows.h>
#include <avrt.h>

#include <string>

#include "Common/settings/Settings.h"
#include "Common/util/Text.h"
#include "OutputBridge/OutputChoices.h"

#pragma comment(lib, "avrt.lib")

namespace camel::audio {

class MmcssScope {
public:
    MmcssScope(MmcssTask task, MmcssPriority priority) {
        const wchar_t* name = mmcssTaskName(task);
        if (!name) return;
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(name, &index);
        if (!handle_) {
            adjustment_ = requestedVsActive("MMCSS task", toUtf8(name), "none (Windows refused it)");
            return;
        }
        const AVRT_PRIORITY value = priority == MmcssPriority::Normal     ? AVRT_PRIORITY_NORMAL
                                    : priority == MmcssPriority::Critical ? AVRT_PRIORITY_CRITICAL
                                                                          : AVRT_PRIORITY_HIGH;
        if (!AvSetMmThreadPriority(handle_, value))
            adjustment_ = requestedVsActive("MMCSS priority", priority == MmcssPriority::Critical ? "critical" : "high",
                                            "normal (Windows refused it)");
    }
    ~MmcssScope() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

    // Empty when the thread runs as configured.
    const std::string& adjustment() const { return adjustment_; }

private:
    HANDLE handle_ = nullptr;
    std::string adjustment_;
};

} // namespace camel::audio
