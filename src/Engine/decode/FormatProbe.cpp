// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/decode/FormatProbe.h"

#include <windows.h>

#include <cstring>
#include <optional>
#include <sstream>
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

namespace {

uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

// The timescale of an mvhd or mdhd full box.
std::optional<uint64_t> timescale(const File& f, const Box& box) {
    uint8_t h[24];
    if (!f.readAt(box.start, h, sizeof h)) return std::nullopt;
    const uint64_t scale = be32(h + (h[0] == 1 ? 20 : 12));
    return scale ? std::optional(scale) : std::nullopt;
}

uint64_t rescale(uint64_t value, uint64_t to, uint64_t from) {
    return (value * to + from / 2) / from;
}

// The single nonempty edit of an audio track: its media start (encoder delay) and duration in movie time.
std::optional<EncoderTrim> editTrim(const File& f, const Box& moov, const Box& trak, const Box& mdia, uint32_t sampleRate) {
    const auto mvhd = findBox(f, moov.start, moov.end, "mvhd");
    const auto mdhd = findBox(f, mdia.start, mdia.end, "mdhd");
    const auto edts = findBox(f, trak.start, trak.end, "edts");
    const auto elst = edts ? findBox(f, edts->start, edts->end, "elst") : std::nullopt;
    if (!mvhd || !mdhd || !elst) return std::nullopt;
    const auto movieScale = timescale(f, *mvhd), mediaScale = timescale(f, *mdhd);
    uint8_t h[8];
    if (!movieScale || !mediaScale || !f.readAt(elst->start, h, sizeof h)) return std::nullopt;
    const bool wide = h[0] == 1;
    const size_t entrySize = wide ? 20 : 12;
    std::optional<EncoderTrim> trim;
    for (uint32_t i = 0, count = be32(h + 4); i < count; i++) {
        uint8_t e[20];
        if (!f.readAt(elst->start + 8 + uint64_t(i) * entrySize, e, entrySize)) return std::nullopt;
        const uint64_t duration = wide ? be64(e) : be32(e);
        const int64_t mediaTime = wide ? int64_t(be64(e + 8)) : int64_t(int32_t(be32(e + 4)));
        if (mediaTime == -1) continue; // an empty edit inserts silence; there is nothing to trim in it
        if (trim || mediaTime < 0) return std::nullopt;
        trim = EncoderTrim{rescale(uint64_t(mediaTime), sampleRate, *mediaScale), rescale(duration, sampleRate, *movieScale)};
    }
    return trim;
}

// iTunes' gapless tag: hex fields " 0 priming padding length ...", in samples.
std::optional<EncoderTrim> itunesTrim(const File& f, const Box& moov) {
    const auto udta = findBox(f, moov.start, moov.end, "udta");
    const auto meta = udta ? findBox(f, udta->start, udta->end, "meta") : std::nullopt;
    const auto ilst = meta ? findBox(f, meta->start + 4, meta->end, "ilst") : std::nullopt; // meta is a full box
    if (!ilst) return std::nullopt;
    for (uint64_t pos = ilst->start;;) {
        const auto item = findBox(f, pos, ilst->end, "----");
        if (!item) return std::nullopt;
        pos = item->end;
        const auto name = findBox(f, item->start, item->end, "name");
        const auto data = findBox(f, item->start, item->end, "data");
        if (!name || !data || name->end - name->start != 4 + 8 || data->end - data->start > 512) continue;
        char key[8];
        if (!f.readAt(name->start + 4, key, sizeof key) || std::memcmp(key, "iTunSMPB", 8) != 0) continue;
        std::string text(size_t(data->end - data->start), '\0');
        if (text.size() <= 8 || !f.readAt(data->start, text.data(), text.size())) return std::nullopt;
        std::istringstream fields(text.substr(8));
        uint64_t zero = 0, priming = 0, padding = 0, length = 0;
        if (!(fields >> std::hex >> zero >> priming >> padding >> length) || !length) return std::nullopt;
        return EncoderTrim{priming, length};
    }
}

} // namespace

bool lossyLengthDeclared(const std::filesystem::path& path) {
    const File f(path);
    uint8_t h[10] = {};
    if (!f.readAt(0, h, sizeof h)) return false;
    if (tagIs(h + 4, "ftyp")) return true;
    uint64_t pos = 0;
    if (h[0] == 'I' && h[1] == 'D' && h[2] == '3')
        pos = 10 +
              ((uint64_t(h[6] & 0x7f) << 21) | (uint64_t(h[7] & 0x7f) << 14) | (uint64_t(h[8] & 0x7f) << 7) |
               uint64_t(h[9] & 0x7f)) +
              ((h[5] & 0x10) ? 10 : 0);
    // The first MPEG frame carries the header after its side information (at most 32 bytes in).
    uint8_t frame[4 + 32 + 4] = {};
    if (!f.readAt(pos, frame, sizeof frame) || frame[0] != 0xFF || (frame[1] & 0xE0) != 0xE0) return false;
    for (size_t i = 4; i + 4 <= sizeof frame; i++)
        if (tagIs(frame + i, "Xing") || tagIs(frame + i, "Info") || tagIs(frame + i, "VBRI")) return true;
    return false;
}

std::optional<EncoderTrim> mp4EncoderTrim(const std::filesystem::path& path, uint32_t sampleRate) {
    const File f(path);
    uint8_t h[8];
    if (!sampleRate || !f.readAt(0, h, sizeof h) || !tagIs(h + 4, "ftyp")) return std::nullopt;
    const auto moov = findBox(f, 0, f.size(), "moov");
    if (!moov) return std::nullopt;
    for (uint64_t pos = moov->start;;) {
        const auto trak = findBox(f, pos, moov->end, "trak");
        if (!trak) break;
        pos = trak->end;
        const auto mdia = findBox(f, trak->start, trak->end, "mdia");
        const auto hdlr = mdia ? findBox(f, mdia->start, mdia->end, "hdlr") : std::nullopt;
        uint8_t handler[12];
        if (!hdlr || !f.readAt(hdlr->start, handler, sizeof handler) || !tagIs(handler + 8, "soun")) continue;
        if (const auto trim = editTrim(f, *moov, *trak, *mdia, sampleRate)) return trim;
        break;
    }
    return itunesTrim(f, *moov);
}

std::optional<uint64_t> aiffSampleFrames(const std::filesystem::path& path) {
    const File f(path);
    uint8_t h[12];
    if (!f.readAt(0, h, sizeof h) || !tagIs(h, "FORM") || !(tagIs(h + 8, "AIFF") || tagIs(h + 8, "AIFC"))) return std::nullopt;
    for (uint64_t pos = 12; f.size() - pos >= 8;) {
        uint8_t chunk[14];
        if (!f.readAt(pos, chunk, 8)) return std::nullopt;
        const uint64_t size = be32(chunk + 4);
        if (tagIs(chunk, "COMM")) {
            if (size < 6 || !f.readAt(pos + 8, chunk + 8, 6)) return std::nullopt;
            return be32(chunk + 10);
        }
        pos += 8 + size + (size & 1); // chunks are padded to even sizes
    }
    return std::nullopt;
}

} // namespace camel::audio
