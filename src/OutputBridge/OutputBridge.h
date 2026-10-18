#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

#include "OutputBridge/IOutput.h"

namespace camel::audio {

class OutputBridge final : public IRenderSource {
public:
    explicit OutputBridge(std::unique_ptr<IOutput> output);
    ~OutputBridge() override;
    OutputBridge(const OutputBridge&) = delete;
    OutputBridge& operator=(const OutputBridge&) = delete;

    bool supports(const DeviceFormat& format);
    void open(const DeviceFormat& format, IRenderSource* source);
    void start();
    void pause();
    void close();
    OutputStatus status() const;
    void setErrorHandler(std::function<void(const Error&)> handler);
    void setRestartHandler(std::function<void()> handler);
    bool showControlPanel();

    // A source that writes its own silence hands the output the whole request.
    size_t render(uint8_t* destination, size_t frames) noexcept override;
    uint64_t generation() const;
    std::optional<DeviceFormat> activeFormat() const;

private:
    bool enterRender() noexcept;
    void leaveRender() noexcept;
    void waitForRenderQuiescence() noexcept;

    static constexpr size_t kClosing = size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    static constexpr size_t kActiveMask = ~kClosing;

    std::unique_ptr<IOutput> output_;
    std::atomic<IRenderSource*> source_{nullptr};
    std::atomic<size_t> renderState_{kClosing};
    mutable std::mutex controlMutex_;
    uint64_t generation_ = 0;
    std::optional<DeviceFormat> activeFormat_;
    bool opened_ = false;
};

} // namespace camel::audio
