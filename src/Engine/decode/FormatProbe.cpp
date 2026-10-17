#include "Engine/decode/FormatProbe.h"

#include <windows.h>

#include <cstring>
#include <optional>
#include <string>

#include "Common/AudioTypes.h"
#include "Common/util/Text.h"

namespace camel::audio {
namespace {

class File {
public:
    explicit File(const std::filesystem::path& path)
        : h_(CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        LARGE_INTEGER size{};
        if (h_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(h_, &size))
            throw Error(ErrorCode::FileOpen, "cannot open " + toUtf8(path.wstring()));
        size_ = uint64_t(size.QuadPart);
    }
    ~File() { CloseHandle(h_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    uint64_t size() const { return size_; }
    bool readAt(uint64_t offset, void* buffer, size_t length) const {
        if (offset > size_ || length > size_ - offset) return false;
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(offset);
        DWORD got = 0;
        return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) && ReadFile(h_, buffer, DWORD(length), &got, nullptr) &&
               got == length;
    }

private:
    HANDLE h_;
    uint64_t size_ = 0;
};

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool tagIs(const uint8_t* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

[[noreturn]] void unsupported(const std::string& detail) {
    throw Error(ErrorCode::UnsupportedStream, detail);
}

struct Box {
    uint64_t start = 0, end = 0; // payload range
    char type[5] = {};
};

// Finds the first child box of `type` within [begin, end).
std::optional<Box> findBox(const File& f, uint64_t begin, uint64_t end, const char* type) {
    uint64_t pos = begin;
    while (end - pos >= 8) {
        uint8_t h[16];
        if (!f.readAt(pos, h, 8)) return std::nullopt;
        uint64_t size = be32(h);
        uint64_t header = 8;
        if (size == 1) {
            if (!f.readAt(pos + 8, h + 8, 8)) return std::nullopt;
            size = (uint64_t(be32(h + 8)) << 32) | be32(h + 12);
            header = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < header || size > end - pos) return std::nullopt;
        if (tagIs(h + 4, type)) {
            Box box{pos + header, pos + size};
            std::memcpy(box.type, h + 4, 4);
            return box;
        }
        pos += size;
    }
    return std::nullopt;
}

// MPEG-4 descriptor length: up to four bytes of 7 bits each.
bool descriptor(const File& f, uint64_t& pos, uint64_t end, uint8_t tag, uint64_t& length) {
    uint8_t b = 0;
    if (pos >= end || !f.readAt(pos, &b, 1) || b != tag) return false;
    ++pos;
    length = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= end || !f.readAt(pos, &b, 1)) return false;
        ++pos;
        length = (length << 7) | (b & 0x7f);
        if (!(b & 0x80)) return length <= end - pos;
    }
    return false;
}

// The esds box selects the codec inside an mp4a sample entry: AAC-LC or MPEG audio.
AudioFormat esdsFormat(const File& f, const Box& esds) {
    uint64_t pos = esds.start + 4; // version and flags
    uint64_t length = 0;
    if (!descriptor(f, pos, esds.end, 0x03, length)) unsupported("MP4 audio has no valid ES descriptor");
    uint8_t es[3];
    if (!f.readAt(pos, es, 3)) unsupported("MP4 audio has no valid ES descriptor");
    pos += 3;
    if (es[2] & 0x80) pos += 2;
    if (es[2] & 0x40) {
        uint8_t urlLength = 0;
        if (!f.readAt(pos, &urlLength, 1)) unsupported("MP4 audio has no valid ES descriptor");
        pos += 1 + urlLength;
    }
    if (es[2] & 0x20) pos += 2;
    if (!descriptor(f, pos, esds.end, 0x04, length) || length < 13) unsupported("MP4 audio has no decoder configuration");
    uint8_t objectType = 0;
    if (!f.readAt(pos, &objectType, 1)) unsupported("MP4 audio has no decoder configuration");
    if (objectType == 0x69 || objectType == 0x6B) return AudioFormat::Mp3;
    if (objectType != 0x40 && objectType != 0x66 && objectType != 0x67 && objectType != 0x68)
        unsupported("MP4 audio object type " + std::to_string(objectType) + " is not supported");
    pos += 13;
    if (!descriptor(f, pos, esds.end, 0x05, length) || length < 1) unsupported("AAC has no audio specific config");
    uint8_t asc[2] = {};
    if (!f.readAt(pos, asc, length >= 2 ? 2 : 1)) unsupported("AAC has no audio specific config");
    unsigned audioObjectType = asc[0] >> 3;
    if (audioObjectType == 31) audioObjectType = 32 + (((asc[0] & 7) << 3) | (asc[1] >> 5));
    if (audioObjectType != 2)
        unsupported("AAC audio object type " + std::to_string(audioObjectType) + " is not supported (AAC-LC only)");
    return AudioFormat::Aac;
}

AudioFormat mp4Format(const File& f) {
    const auto moov = findBox(f, 0, f.size(), "moov");
    if (!moov) unsupported("MP4 file has no movie box");
    for (uint64_t pos = moov->start;;) {
        const auto trak = findBox(f, pos, moov->end, "trak");
        if (!trak) break;
        pos = trak->end;
        const auto mdia = findBox(f, trak->start, trak->end, "mdia");
        if (!mdia) continue;
        const auto hdlr = findBox(f, mdia->start, mdia->end, "hdlr");
        uint8_t handler[12];
        if (!hdlr || !f.readAt(hdlr->start, handler, sizeof handler) || !tagIs(handler + 8, "soun")) continue;
        const auto minf = findBox(f, mdia->start, mdia->end, "minf");
        const auto stbl = minf ? findBox(f, minf->start, minf->end, "stbl") : std::nullopt;
        const auto stsd = stbl ? findBox(f, stbl->start, stbl->end, "stsd") : std::nullopt;
        uint8_t entry[8 + 8 + 2];
        if (!stsd || !f.readAt(stsd->start + 8, entry, sizeof entry)) unsupported("MP4 audio track has no sample description");
        const uint64_t entryStart = stsd->start + 8;
        const uint64_t entryEnd = entryStart + be32(entry);
        if (be32(entry) < 36 || entryEnd > stsd->end) unsupported("MP4 audio track has an invalid sample description");
        if (tagIs(entry + 4, "alac")) return AudioFormat::Alac;
        if (!tagIs(entry + 4, "mp4a"))
            unsupported("MP4 codec '" + std::string(reinterpret_cast<const char*>(entry + 4), 4) + "' is not supported");
        // QuickTime sound description versions 1 and 2 extend the fixed fields before the child boxes.
        const unsigned version = (unsigned(entry[16]) << 8) | entry[17];
        const uint64_t children = entryStart + (version == 1 ? 52 : version == 2 ? 72 : 36);
        if (children > entryEnd) unsupported("MP4 audio track has an invalid sample description");
        auto esds = findBox(f, children, entryEnd, "esds");
        if (!esds)
            if (const auto wave = findBox(f, children, entryEnd, "wave")) esds = findBox(f, wave->start, wave->end, "esds");
        if (!esds) unsupported("MP4 audio track has no codec configuration");
        return esdsFormat(f, *esds);
    }
    unsupported("MP4 file has no audio track");
}

} // namespace

AudioFormat probeFormat(const std::filesystem::path& path) {
    const File f(path);
    uint8_t h[12] = {};
    if (!f.readAt(0, h, sizeof h)) unsupported("file is too short to identify");
    if (tagIs(h + 4, "ftyp")) return mp4Format(f);
    if (tagIs(h, "RIFF") || tagIs(h, "RF64")) {
        if (tagIs(h + 8, "WAVE")) return AudioFormat::Wav;
        unsupported("RIFF file is not WAVE");
    }
    if (tagIs(h, "FORM")) {
        if (tagIs(h + 8, "AIFF") || tagIs(h + 8, "AIFC")) return AudioFormat::Aiff;
        unsupported("IFF file is not AIFF");
    }
    if (tagIs(h, "DSD ") || tagIs(h, "FRM8")) return AudioFormat::Dsd;

    uint64_t pos = 0;
    if (h[0] == 'I' && h[1] == 'D' && h[2] == '3') { // ID3v2: synchsafe size, optional footer
        pos = 10 + ((uint64_t(h[6] & 0x7f) << 21) | (uint64_t(h[7] & 0x7f) << 14) | (uint64_t(h[8] & 0x7f) << 7) |
                    uint64_t(h[9] & 0x7f));
        if (h[5] & 0x10) pos += 10;
        if (!f.readAt(pos, h, 4)) unsupported("file has an ID3 tag but no audio");
    }
    if (tagIs(h, "fLaC")) return AudioFormat::Flac;
    if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) {
        const unsigned layer = (h[1] >> 1) & 3;
        if (layer == 0 && (h[1] & 0x10)) { // ADTS: profile is the audio object type minus one
            if ((h[2] >> 6) != 1)
                unsupported("ADTS AAC profile " + std::to_string(h[2] >> 6) + " is not supported (AAC-LC only)");
            return AudioFormat::Aac;
        }
        if (layer == 1 && ((h[1] >> 3) & 3) != 1) return AudioFormat::Mp3;
        unsupported("MPEG audio layer I/II is not supported");
    }
    unsupported("unrecognized audio format");
}

} // namespace camel::audio
