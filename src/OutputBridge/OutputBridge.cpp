#include "OutputBridge/OutputBridge.h"

#include <windows.h>

#include <thread>
#include <utility>

#include "OutputBridge/AsioOutput.h"

namespace camel::audio {

OutputBridge::OutputBridge(std::unique_ptr<IOutput> output) : output_(std::move(output)) {
    if (!output_) throw Error(ErrorCode::Other, "output bridge requires an output backend");
}

OutputBridge::~OutputBridge() {
    try {
        close();
    } catch (...) {
        OutputDebugStringW(L"OutputBridge: failed to close output during destruction.\n");
    }
}

bool OutputBridge::supports(const DeviceFormat& format) {
    std::lock_guard lock(controlMutex_);
    return output_->supports(format);
}

void OutputBridge::open(const DeviceFormat& format, IRenderSource* source) {
    if (!source) throw Error(ErrorCode::Other, "output bridge requires a render source");
    std::lock_guard lock(controlMutex_);
    if (opened_) throw Error(ErrorCode::Other, "output bridge stream is already open");

    source_.store(source, std::memory_order_release);
    renderState_.store(0, std::memory_order_release);
    try {
        output_->open(format, this);
    } catch (...) {
        renderState_.fetch_or(kClosing, std::memory_order_acq_rel);
        try {
            output_->close();
        } catch (...) {
            OutputDebugStringW(L"OutputBridge: failed to close output after open failure.\n");
        }
        waitForRenderQuiescence();
        source_.store(nullptr, std::memory_order_release);
        throw;
    }
    ++generation_;
    activeFormat_ = format;
    opened_ = true;
}

void OutputBridge::start() {
    std::lock_guard lock(controlMutex_);
    if (!opened_) throw Error(ErrorCode::Other, "output bridge stream is not open");
    output_->start();
}

void OutputBridge::pause() {
    std::lock_guard lock(controlMutex_);
    if (!opened_) throw Error(ErrorCode::Other, "output bridge stream is not open");
    output_->pause();
}

void OutputBridge::close() {
    std::lock_guard lock(controlMutex_);
    if (!opened_) return;

    renderState_.fetch_or(kClosing, std::memory_order_acq_rel);
    try {
        output_->close();
    } catch (...) {
        waitForRenderQuiescence();
        source_.store(nullptr, std::memory_order_release);
        activeFormat_.reset();
        opened_ = false;
        throw;
    }
    waitForRenderQuiescence();
    source_.store(nullptr, std::memory_order_release);
    activeFormat_.reset();
    opened_ = false;
}

OutputStatus OutputBridge::status() const {
    std::lock_guard lock(controlMutex_);
    return output_->status();
}

void OutputBridge::setErrorHandler(std::function<void(const Error&)> handler) {
    std::lock_guard lock(controlMutex_);
    output_->setErrorHandler(std::move(handler));
}

void OutputBridge::setRestartHandler(std::function<void()> handler) {
    std::lock_guard lock(controlMutex_);
    output_->setRestartHandler(std::move(handler));
}

bool OutputBridge::showControlPanel() {
    std::lock_guard lock(controlMutex_);
    return showAsioControlPanel(output_.get());
}

size_t OutputBridge::render(uint8_t* destination, size_t frames) noexcept {
    if (!enterRender()) return 0;
    IRenderSource* source = source_.load(std::memory_order_acquire);
    const size_t rendered = source ? source->render(destination, frames) : 0;
    leaveRender();
    return rendered;
}

uint64_t OutputBridge::generation() const {
    std::lock_guard lock(controlMutex_);
    return generation_;
}

std::optional<DeviceFormat> OutputBridge::activeFormat() const {
    std::lock_guard lock(controlMutex_);
    return activeFormat_;
}

bool OutputBridge::enterRender() noexcept {
    size_t state = renderState_.load(std::memory_order_acquire);
    for (;;) {
        if ((state & kClosing) != 0 || (state & kActiveMask) == kActiveMask) return false;
        if (renderState_.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
}

void OutputBridge::leaveRender() noexcept {
    renderState_.fetch_sub(1, std::memory_order_release);
}

void OutputBridge::waitForRenderQuiescence() noexcept {
    while ((renderState_.load(std::memory_order_acquire) & kActiveMask) != 0) std::this_thread::yield();
}

} // namespace camel::audio
