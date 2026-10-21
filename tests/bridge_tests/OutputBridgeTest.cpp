// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "OutputBridge/OutputBridge.h"

using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

struct BridgeProbe {
    IRenderSource* callback = nullptr;
    DeviceFormat openedFormat;
    std::mutex mutex;
    std::condition_variable closedCondition;
    int opens = 0;
    int starts = 0;
    int pauses = 0;
    int closes = 0;
};

class BridgeTestOutput final : public IOutput {
public:
    explicit BridgeTestOutput(BridgeProbe& probe) : probe_(probe) {}

    bool supports(const DeviceFormat&) override { return true; }

    void open(const DeviceFormat& format, IRenderSource* source) override {
        ++probe_.opens;
        probe_.openedFormat = format;
        probe_.callback = source;
    }

    void start() override { ++probe_.starts; }
    void pause() override { ++probe_.pauses; }

    void close() override {
        {
            std::lock_guard lock(probe_.mutex);
            ++probe_.closes;
        }
        probe_.closedCondition.notify_all();
    }

    OutputStatus status() const override { return {"Bridge test output", 64, {}}; }

private:
    BridgeProbe& probe_;
};

class CountingSource final : public IRenderSource {
public:
    size_t render(uint8_t*, size_t frames) noexcept override {
        ++calls;
        return frames;
    }

    std::atomic<int> calls = 0;
};

class BlockingSource final : public IRenderSource {
public:
    size_t render(uint8_t*, size_t frames) noexcept override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
        return frames;
    }

    bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 1s, [&] { return entered_; });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

TEST(OutputBridge, UsesFixedFormatPerGenerationAndForwardsControls) {
    BridgeProbe probe;
    OutputBridge bridge(std::make_unique<BridgeTestOutput>(probe));
    CountingSource source;
    const DeviceFormat first{48000, 2, SampleType::Int16};
    const DeviceFormat second{96000, 2, SampleType::Int24In32};

    bridge.open(first, &source);
    EXPECT_EQ(bridge.generation(), 1u);
    EXPECT_EQ(bridge.activeFormat(), first);
    EXPECT_EQ(probe.openedFormat, first);
    EXPECT_EQ(probe.callback, &bridge);
    EXPECT_TRUE(bridge.supports(second));
    EXPECT_EQ(bridge.render(nullptr, 17), 17u);
    EXPECT_EQ(source.calls.load(), 1);
    bridge.start();
    bridge.pause();
    EXPECT_EQ(probe.starts, 1);
    EXPECT_EQ(probe.pauses, 1);
    EXPECT_THROW(bridge.open(second, &source), Error);

    bridge.close();
    EXPECT_FALSE(bridge.activeFormat());
    EXPECT_EQ(bridge.render(nullptr, 17), 0u);

    bridge.open(second, &source);
    EXPECT_EQ(bridge.generation(), 2u);
    EXPECT_EQ(bridge.activeFormat(), second);
    EXPECT_EQ(probe.openedFormat, second);
    bridge.close();
    EXPECT_EQ(probe.opens, 2);
    EXPECT_EQ(probe.closes, 2);
}

TEST(OutputBridge, CloseWaitsForInFlightRenderBeforeReleasingSource) {
    BridgeProbe probe;
    OutputBridge bridge(std::make_unique<BridgeTestOutput>(probe));
    BlockingSource source;
    bridge.open({48000, 2, SampleType::Int16}, &source);

    std::thread callback([&] { EXPECT_EQ(bridge.render(nullptr, 1), 1u); });
    if (!source.waitUntilEntered()) {
        source.release();
        callback.join();
        FAIL() << "Render callback did not enter the source";
        return;
    }

    auto close = std::async(std::launch::async, [&] { bridge.close(); });
    bool closeCalled = false;
    {
        std::unique_lock lock(probe.mutex);
        closeCalled = probe.closedCondition.wait_for(lock, 1s, [&] { return probe.closes == 1; });
    }
    if (!closeCalled) {
        source.release();
        callback.join();
        close.wait();
        FAIL() << "Output backend close was not called";
        return;
    }
    const auto pendingStatus = close.wait_for(20ms);

    source.release();
    callback.join();
    EXPECT_EQ(pendingStatus, std::future_status::timeout);
    EXPECT_EQ(close.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(bridge.render(nullptr, 1), 0u);
}

} // namespace
