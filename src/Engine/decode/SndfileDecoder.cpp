// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// FLAC, WAV and AIFF through libsndfile (which decodes FLAC with libFLAC internally).
#include "Engine/decode/DecoderImpl.h"

#include <windows.h>
#include <sndfile.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/util/Text.h"

namespace camel::audio {
namespace {

// libsndfile reads the file through this view, which leaves out a FLAC's PICTURE and PADDING blocks: libFLAC reads
// through every metadata block it does not need, and from a NAS at about 2 MB/s a 2.5 MB cover delayed the start of
// playback by 1.9 s (docs/Plan.md §12.1). The view is the rebuilt metadata, then the file from its first audio frame on,
// so seeking and the audio MD5 are unchanged. Other files are passed through. The file is read in 64 KiB requests, as
// each one costs a network round trip.
class FileView {
public:
    explicit FileView(const std::filesystem::path& path)
        : h_(CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr)),
          buffer_(64 * 1024) {
        LARGE_INTEGER size{};
        if (h_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(h_, &size)) {
            if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
            throw Error(ErrorCode::FileOpen, "cannot open " + toUtf8(path.wstring()));
        }
        fileSize_ = size.QuadPart;
        dropPictures();
        size_ = sf_count_t(header_.size()) + fileSize_ - body_;
    }
    ~FileView() { CloseHandle(h_); }
    FileView(const FileView&) = delete;
    FileView& operator=(const FileView&) = delete;

    static SF_VIRTUAL_IO io() {
        SF_VIRTUAL_IO io{};
        io.get_filelen = [](void* self) { return static_cast<FileView*>(self)->size_; };
        io.seek = [](sf_count_t offset, int whence, void* self) {
            auto& view = *static_cast<FileView*>(self);
            const sf_count_t base = whence == SEEK_CUR ? view.position_ : whence == SEEK_END ? view.size_ : 0;
            view.position_ = std::clamp<sf_count_t>(base + offset, 0, view.size_);
            return view.position_;
        };
        io.read = [](void* dst, sf_count_t count, void* self) { return static_cast<FileView*>(self)->read(dst, count); };
        io.write = [](const void*, sf_count_t, void*) { return sf_count_t(0); };
        io.tell = [](void* self) { return static_cast<FileView*>(self)->position_; };
        return io;
    }

private:
    // Copies file bytes from offset through the buffer; fewer than asked at the end of the file or on a read error.
    sf_count_t copy(sf_count_t offset, uint8_t* dst, sf_count_t length) {
        sf_count_t done = 0;
        while (done < length) {
            if (offset < start_ || offset >= start_ + filled_) {
                DWORD got = 0;
                if (!SetFilePointerEx(h_, LARGE_INTEGER{.QuadPart = offset}, nullptr, FILE_BEGIN) ||
                    !ReadFile(h_, buffer_.data(), DWORD(buffer_.size()), &got, nullptr) || !got)
                    break;
                start_ = offset;
                filled_ = got;
            }
            const sf_count_t n = std::min(length - done, start_ + filled_ - offset);
            std::memcpy(dst + done, buffer_.data() + (offset - start_), size_t(n));
            done += n;
            offset += n;
        }
        return done;
    }

    // Keeps the file as it is unless it is a native FLAC (after an optional ID3v2 tag) with blocks to leave out.
    void dropPictures() {
        uint8_t head[10];
        if (copy(0, head, sizeof head) != sizeof head) return;
        sf_count_t at = 0;
        if (std::memcmp(head, "ID3", 3) == 0)
            at = 10 + (sf_count_t(head[6] & 0x7f) << 21 | sf_count_t(head[7] & 0x7f) << 14 | sf_count_t(head[8] & 0x7f) << 7 |
                       (head[9] & 0x7f));
        uint8_t marker[4];
        if (copy(at, marker, 4) != 4 || std::memcmp(marker, "fLaC", 4) != 0) return;
        std::vector<uint8_t> kept{'f', 'L', 'a', 'C'};
        size_t lastHeader = 0;
        bool dropped = false;
        for (bool last = false; !last;) {
            uint8_t block[4];
            if (copy(at += 4, block, 4) != 4) return;
            last = block[0] & 0x80;
            const sf_count_t length = sf_count_t(block[1]) << 16 | sf_count_t(block[2]) << 8 | block[3];
            const uint8_t type = block[0] & 0x7f;
            if (type == 1 || type == 6) {
                dropped = true;
            } else {
                lastHeader = kept.size();
                kept.insert(kept.end(), block, block + 4);
                kept[lastHeader] &= 0x7f;
                kept.resize(kept.size() + size_t(length));
                if (copy(at + 4, kept.data() + kept.size() - size_t(length), length) != length) return;
            }
            at += length;
        }
        if (!dropped || kept.size() == 4) return;
        kept[lastHeader] |= 0x80;
        header_ = std::move(kept);
        body_ = at + 4;
    }

    sf_count_t read(void* dst, sf_count_t count) {
        auto* out = static_cast<uint8_t*>(dst);
        const auto headerSize = sf_count_t(header_.size());
        const sf_count_t fromHeader = std::clamp<sf_count_t>(headerSize - position_, 0, count);
        std::memcpy(out, header_.data() + std::min(position_, headerSize), size_t(fromHeader));
        position_ += fromHeader;
        const sf_count_t rest = std::min(count - fromHeader, size_ - position_);
        if (rest <= 0) return fromHeader;
        const sf_count_t done = copy(body_ + position_ - headerSize, out + fromHeader, rest);
        position_ += done;
        return fromHeader + done;
    }

    HANDLE h_;
    std::vector<uint8_t> buffer_, header_;
    sf_count_t fileSize_ = 0, body_ = 0, size_ = 0, position_ = 0, start_ = 0, filled_ = 0;
};

class SndfileDecoder final : public IDecoder {
public:
    explicit SndfileDecoder(const std::filesystem::path& path) : file_(path) {
        SF_INFO si{};
        SF_VIRTUAL_IO io = FileView::io();
        sf_ = sf_open_virtual(&io, SFM_READ, &si, &file_);
        // A readable file that libsndfile cannot decode (e.g. 20/32-bit FLAC) is an unsupported stream, not an I/O error.
        if (!sf_) throw Error(ErrorCode::UnsupportedStream, std::string("sf_open_virtual: ") + sf_strerror(nullptr));
        switch (si.format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_16:
            info_.bitsPerSample = 16;
            break;
        case SF_FORMAT_PCM_24:
            info_.bitsPerSample = 24;
            break;
        default:
            sf_close(sf_);
            throw Error(ErrorCode::UnsupportedStream, "libsndfile: unsupported sample format");
        }
        info_.sampleRate = uint32_t(si.samplerate);
        info_.channels = uint32_t(si.channels);
        info_.totalFrames = uint64_t(si.frames);
    }

    ~SndfileDecoder() override { sf_close(sf_); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        sf_count_t got = sf_readf_int(sf_, dst, sf_count_t(frames));
        if (sf_error(sf_) != SF_ERR_NO_ERROR)
            throw Error(ErrorCode::DecodeFailed, std::string("libsndfile: ") + sf_strerror(sf_));
        // libsndfile scales integer reads to full 32-bit range; bring them back to the original bits.
        const int shift = 32 - int(info_.bitsPerSample);
        if (shift)
            for (size_t i = 0, n = size_t(got) * info_.channels; i < n; i++) dst[i] >>= shift;
        return size_t(got);
    }

    void seek(uint64_t frame) override {
        if (sf_seek(sf_, sf_count_t(frame), SEEK_SET) < 0)
            throw Error(ErrorCode::SeekFailed, std::string("sf_seek: ") + sf_strerror(sf_));
    }

private:
    FileView file_; // outlives sf_, which reads through it
    SNDFILE* sf_ = nullptr;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openSndfile(const std::filesystem::path& path) {
    return std::make_unique<SndfileDecoder>(path);
}

} // namespace camel::audio
