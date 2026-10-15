#include "audio/output/CaptureOutput.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace camel::audio {
namespace {

class CaptureOutput final : public IOutput {
public:
    explicit CaptureOutput(CaptureOptions o) : opts_(std::move(o)) {}
    ~CaptureOutput() override { close(); }

    bool supports(const DeviceFormat& f) override {
        if (opts_.accepts) return opts_.accepts(f);
        return f.type != SampleType::Float32;
    }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        close();
        if (_wfopen_s(&file_, opts_.file.c_str(), L"wb") != 0 || !file_) throw Error(ErrorCode::DeviceNotFound, "cannot create capture file");
        fmt_ = f;
        source_ = source;
        quit_ = false;
        running_ = false;
        thread_ = std::thread([this] { run(); });
    }

    void start() override { running_ = true; }
    void pause() override { running_ = false; }

    void close() override {
        if (thread_.joinable()) {
            quit_ = true;
            thread_.join();
        }
        if (file_) fclose(file_);
        file_ = nullptr;
    }

    OutputStatus status() const override { return {"Capture", kChunk, opts_.file.string()}; }

private:
    static constexpr uint32_t kChunk = 4096;

    void run() {
        std::vector<uint8_t> buf(size_t(kChunk) * fmt_.bytesPerFrame());
        auto t0 = std::chrono::steady_clock::now();
        uint64_t frames = 0;
        while (!quit_) {
            if (!running_) {
                Sleep(1);
                continue;
            }
            if (opts_.realtime) {
                auto due = t0 + std::chrono::duration<double>(double(frames) / fmt_.sampleRate);
                std::this_thread::sleep_until(due);
            }
            size_t got = source_->render(buf.data(), kChunk);
            if (got) {
                fwrite(buf.data(), fmt_.bytesPerFrame(), got, file_);
                frames += got;
            } else {
                Sleep(1);
            }
        }
    }

    CaptureOptions opts_;
    DeviceFormat fmt_;
    IRenderSource* source_ = nullptr;
    FILE* file_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
};

}  // namespace

std::unique_ptr<IOutput> createCaptureOutput(CaptureOptions opts) { return std::make_unique<CaptureOutput>(std::move(opts)); }

}  // namespace camel::audio
