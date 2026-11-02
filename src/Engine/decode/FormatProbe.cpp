// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/decode/FormatProbe.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "Common/AudioTypes.h"
#include "Common/DecoderRegistry.h"
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

uint32_t le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

bool tagIs(const uint8_t* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

// Where the payload of a WAVE file's first `id` chunk starts; nullopt when it is missing or a chunk before it is damaged.
std::optional<uint64_t> waveChunk(const File& f, const char* id) {
    for (uint64_t pos = 12; f.size() - pos >= 8;) {
        uint8_t chunk[8];
        if (!f.readAt(pos, chunk, sizeof chunk)) return std::nullopt;
        if (tagIs(chunk, id)) return pos + 8;
        const uint64_t size = le32(chunk + 4);
        if (size > f.size() - pos - 8) return std::nullopt;
        pos += 8 + size + (size & 1);
    }
    return std::nullopt;
}

// Microsoft (0x0002) and IMA (0x0011) ADPCM, whose 4-bit codes decode to 16-bit samples.
bool adpcmWave(const File& f) {
    const auto fmt = waveChunk(f, "fmt ");
    uint8_t tag[2];
    if (!fmt || !f.readAt(*fmt, tag, sizeof tag)) return false;
    const unsigned formatTag = tag[0] | (unsigned(tag[1]) << 8);
    return formatTag == 0x0002 || formatTag == 0x0011;
}

// DTS-CD stores a DTS bitstream as 16-bit PCM, usually packed 14 bits to a word, which a PCM decoder plays as loud
// noise. The data chunk then starts with a DTS sync word (14- or 16-bit, either byte order) repeating every frame;
// three at one spacing rule out PCM that happens to contain the pattern.
bool dtsInWave(const File& f) {
    const auto data = waveChunk(f, "data");
    if (!data) return false;
    std::string window(size_t(std::min<uint64_t>(32768, f.size() - *data)), '\0');
    if (!f.readAt(*data, window.data(), window.size())) return false;
    static constexpr std::string_view kSyncs[] = {
        {"\xFF\x1F\x00\xE8", 4}, {"\x1F\xFF\xE8\x00", 4}, {"\xFE\x7F\x01\x80", 4}, {"\x7F\xFE\x80\x01", 4}};
    const auto nextWord = [&](std::string_view sync, size_t from) {
        size_t at = window.find(sync, from);
        while (at != std::string::npos && at % 2) at = window.find(sync, at + 1);
        return at;
    };
    for (const auto sync : kSyncs) {
        const size_t first = nextWord(sync, 0);
        if (first == std::string::npos) continue;
        const size_t second = nextWord(sync, first + 2);
        if (second == std::string::npos) continue;
        const size_t third = 2 * second - first;
        if (third + sync.size() <= window.size() && window.compare(third, sync.size(), sync) == 0) return true;
    }
    return false;
}

[[noreturn]] void unsupported(const std::string& detail) {
    throw Error(ErrorCode::UnsupportedStream, detail);
}

// Where the data after the ID3v2 tags at the start of the file begins; some taggers leave two tags in a row.
uint64_t afterId3Tags(const File& f) {
    uint64_t pos = 0;
    for (uint8_t h[10]; f.readAt(pos, h, sizeof h) && h[0] == 'I' && h[1] == 'D' && h[2] == '3';)
        pos += 10 +
               ((uint64_t(h[6] & 0x7f) << 21) | (uint64_t(h[7] & 0x7f) << 14) | (uint64_t(h[8] & 0x7f) << 7) |
                uint64_t(h[9] & 0x7f)) +
               ((h[5] & 0x10) ? 10 : 0); // synchsafe size, optional footer
    return pos;
}

bool frameSync(const uint8_t* h) {
    return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0;
}

// The length of the MPEG audio frame (layer I, II or III) whose header starts at `h`; 0 for anything else.
uint32_t mpegFrameLength(const uint8_t* h) {
    static constexpr uint16_t kBitrates[2][3][15] = {// kbit/s: MPEG-1, then MPEG-2 and 2.5; layers I to III
                                                     {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                                                      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                                                      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
                                                     {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
                                                      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                                                      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    static constexpr uint32_t kRates[3] = {44100, 48000, 32000};
    const unsigned version = (h[1] >> 3) & 3, layerBits = (h[1] >> 1) & 3, bitrateIndex = h[2] >> 4, rateIndex = (h[2] >> 2) & 3;
    if (!frameSync(h) || version == 1 || layerBits == 0 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return 0;
    const bool mpeg1 = version == 3;
    const unsigned layer = 4 - layerBits;
    const uint32_t bitrate = kBitrates[mpeg1 ? 0 : 1][layer - 1][bitrateIndex] * 1000u;
    const uint32_t rate = kRates[rateIndex] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
    const uint32_t padding = (h[2] >> 1) & 1;
    if (layer == 1) return (12 * bitrate / rate + padding) * 4;
    return (layer == 3 && !mpeg1 ? 72 : 144) * bitrate / rate + padding;
}

// MPEG audio that padding or other data separates from the tags before it: the first frame header within 64 KiB
// that the next frame's header confirms (same version, layer and sampling rate).
std::optional<uint64_t> confirmedMpegFrame(const File& f, uint64_t pos) {
    constexpr size_t kSearch = 64 * 1024, kLongestFrame = 2881;
    if (pos >= f.size()) return std::nullopt;
    std::string window(size_t(std::min<uint64_t>(kSearch + kLongestFrame + 4, f.size() - pos)), '\0');
    if (!f.readAt(pos, window.data(), window.size())) return std::nullopt;
    const auto* b = reinterpret_cast<const uint8_t*>(window.data());
    for (size_t at = 0; at < kSearch && at + 4 <= window.size(); at++) {
        const uint32_t length = mpegFrameLength(b + at);
        if (!length || at + length + 4 > window.size()) continue;
        const uint8_t* next = b + at + length;
        if (mpegFrameLength(next) && (next[1] & 0xFE) == (b[at + 1] & 0xFE) && (next[2] & 0x0C) == (b[at + 2] & 0x0C))
            return pos + at;
    }
    return std::nullopt;
}

uint64_t mpegStart(const File& f) {
    const uint64_t pos = afterId3Tags(f);
    uint8_t h[4];
    if (!pos || (f.readAt(pos, h, sizeof h) && frameSync(h))) return pos;
    return confirmedMpegFrame(f, pos).value_or(pos);
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

// The built-in content rules; nullopt when no rule matches.
std::optional<AudioFormat> builtinFormat(const std::filesystem::path& path) {
    const File f(path);
    uint8_t h[12] = {};
    if (!f.readAt(0, h, sizeof h)) unsupported("file is too short to identify");
    if (tagIs(h + 4, "ftyp")) return mp4Format(f);
    if (tagIs(h, "RIFF") || tagIs(h, "RF64")) {
        if (tagIs(h + 8, "WAVE")) return adpcmWave(f) ? AudioFormat::Adpcm : dtsInWave(f) ? AudioFormat::Dts : AudioFormat::Wav;
        unsupported("RIFF file is not WAVE");
    }
    if (tagIs(h, "FORM")) {
        if (tagIs(h + 8, "AIFF") || tagIs(h + 8, "AIFC")) return AudioFormat::Aiff;
        unsupported("IFF file is not AIFF");
    }
    if (tagIs(h, "DSD ") || tagIs(h, "FRM8")) return AudioFormat::Dsd;

    const uint64_t pos = afterId3Tags(f);
    if (pos && !f.readAt(pos, h, 4)) unsupported("file has an ID3 tag but no audio");
    if (tagIs(h, "fLaC")) return AudioFormat::Flac;
    if (pos && !frameSync(h))
        if (const auto frame = confirmedMpegFrame(f, pos); !frame || !f.readAt(*frame, h, 4)) return std::nullopt;
    if (frameSync(h)) {
        const unsigned layer = (h[1] >> 1) & 3;
        if (layer == 0 && (h[1] & 0x10)) { // ADTS: profile is the audio object type minus one
            if ((h[2] >> 6) != 1)
                unsupported("ADTS AAC profile " + std::to_string(h[2] >> 6) + " is not supported (AAC-LC only)");
            return AudioFormat::Aac;
        }
        if (layer == 1 && ((h[1] >> 3) & 3) != 1) return AudioFormat::Mp3;
        unsupported("MPEG audio layer I/II is not supported");
    }
    return std::nullopt;
}

} // namespace

AudioFormat probeFormat(const std::filesystem::path& path) {
    static std::once_flag once;
    std::call_once(once, [] {
        decoderRegistry().addProbe({"builtin", [](const std::filesystem::path& p) -> std::optional<std::string> {
                                        const auto format = builtinFormat(p);
                                        return format ? std::optional<std::string>(formatId(*format)) : std::nullopt;
                                    }});
    });
    for (const auto& probe : decoderRegistry().probes())
        if (const auto id = probe.identify(path)) {
            AudioFormat format{};
            if (formatFromId(*id, format)) return format;
            unsupported("format " + *id + " has no player support yet");
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
    // The first MPEG frame carries the header after its side information (at most 32 bytes in).
    uint8_t frame[4 + 32 + 4] = {};
    if (!f.readAt(mpegStart(f), frame, sizeof frame) || !frameSync(frame)) return false;
    for (size_t i = 4; i + 4 <= sizeof frame; i++)
        if (tagIs(frame + i, "Xing") || tagIs(frame + i, "Info")) return true;
    return false;
}

uint64_t vbriSamples(const std::filesystem::path& path) {
    const File f(path);
    // The VBRI frame follows the header and 32 bytes of side information: version, delay, quality, bytes, frames.
    uint8_t frame[36 + 18] = {};
    if (!f.readAt(mpegStart(f), frame, sizeof frame) || !frameSync(frame) || !tagIs(frame + 36, "VBRI")) return 0;
    const bool mpeg1 = ((frame[1] >> 3) & 3) == 3;
    const uint32_t frames = be32(frame + 36 + 14); // the VBRI frame counts itself
    return uint64_t(frames ? frames - 1 : 0) * (mpeg1 ? 1152 : 576);
}

std::optional<XingToc> xingToc(const std::filesystem::path& path) {
    const File f(path);
    // The tag follows the side information (at most 32 bytes in): flags, then frames, bytes and the table as flagged.
    uint8_t frame[4 + 32 + 8 + 8 + 100] = {};
    if (!f.readAt(mpegStart(f), frame, sizeof frame) || !frameSync(frame)) return std::nullopt;
    for (size_t i = 4; i <= 36; i++) {
        if (!tagIs(frame + i, "Xing")) continue;
        if ((be32(frame + i + 4) & 7) != 7) return std::nullopt;
        XingToc xing;
        xing.frames = be32(frame + i + 8);
        xing.bytes = be32(frame + i + 12);
        std::copy_n(frame + i + 16, xing.toc.size(), xing.toc.begin());
        if (!xing.frames || !xing.bytes) return std::nullopt;
        return xing;
    }
    return std::nullopt;
}

uint64_t audioAfterVbriFrame(const std::filesystem::path& path) {
    const File f(path);
    const uint64_t start = mpegStart(f);
    uint8_t frame[40] = {};
    if (!f.readAt(start, frame, sizeof frame) || !tagIs(frame + 36, "VBRI")) return 0;
    const uint32_t length = mpegFrameLength(frame);
    return length ? start + length : 0;
}

uint64_t mpegAudioStart(const std::filesystem::path& path) {
    return mpegStart(File(path));
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
