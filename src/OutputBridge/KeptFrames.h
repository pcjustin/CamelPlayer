// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The frames an output last wrote to its device, so a pause that empties the device can write the ones it had not played
// first when it resumes. Sized at open; nothing allocates on the real-time thread.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace camel::audio {

class KeptFrames {
public:
    // Room for the device's whole buffer of `frames` frames.
    void reset(size_t frames, size_t bytesPerFrame) {
        bytesPerFrame_ = bytesPerFrame;
        capacity_ = frames;
        history_.assign(frames * bytesPerFrame, 0);
        kept_.assign(frames * bytesPerFrame, 0);
        end_ = filled_ = keptFrames_ = keptAt_ = 0;
    }

    // `frames` frames were just written to the device.
    void wrote(const uint8_t* data, size_t frames) noexcept {
        if (!capacity_) return;
        if (frames >= capacity_) {
            data += (frames - capacity_) * bytesPerFrame_;
            frames = capacity_;
        }
        const size_t first = std::min(frames, capacity_ - end_);
        std::memcpy(history_.data() + end_ * bytesPerFrame_, data, first * bytesPerFrame_);
        std::memcpy(history_.data(), data + first * bytesPerFrame_, (frames - first) * bytesPerFrame_);
        end_ = (end_ + frames) % capacity_;
        filled_ = std::min(capacity_, filled_ + frames);
    }

    // The device held the last `frames` written when it was emptied: they are kept to be written again.
    void unplayed(size_t frames) noexcept {
        if (!capacity_) return;
        frames = std::min(frames, filled_);
        const size_t start = (end_ + capacity_ - frames) % capacity_;
        const size_t first = std::min(frames, capacity_ - start);
        std::memcpy(kept_.data(), history_.data() + start * bytesPerFrame_, first * bytesPerFrame_);
        std::memcpy(kept_.data() + first * bytesPerFrame_, history_.data(), (frames - first) * bytesPerFrame_);
        keptFrames_ = frames;
        keptAt_ = 0;
        filled_ = 0;
    }

    // Writes up to `frames` kept frames to `dst`; returns how many.
    size_t take(uint8_t* dst, size_t frames) noexcept {
        const size_t n = std::min(frames, keptFrames_ - keptAt_);
        if (!n) return 0;
        std::memcpy(dst, kept_.data() + keptAt_ * bytesPerFrame_, n * bytesPerFrame_);
        keptAt_ += n;
        return n;
    }

private:
    size_t bytesPerFrame_ = 0;
    size_t capacity_ = 0;
    std::vector<uint8_t> history_; // a ring of the last `capacity_` frames written
    size_t end_ = 0;               // where the next frame written goes
    size_t filled_ = 0;            // frames of the ring that were written since the last pause
    std::vector<uint8_t> kept_;
    size_t keptFrames_ = 0;
    size_t keptAt_ = 0;
};

} // namespace camel::audio
