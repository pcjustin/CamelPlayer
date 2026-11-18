// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/Plist.h"

#include <bit>
#include <stdexcept>
#include <utility>

#include "Common/util/Text.h"

namespace camel::server::airplay {

namespace {

constexpr std::string_view kMagic = "bplist00";
constexpr size_t kTrailerBytes = 32;
constexpr int kMaxDepth = 32;
constexpr uint64_t kMaxObjects = 4096;
constexpr uint64_t kMaxVisits = 16384; // objects shared between containers are read once for each
constexpr uint8_t kFalse = 0x08;
constexpr uint8_t kTrue = 0x09;
constexpr uint8_t kInteger = 0x10;
constexpr uint8_t kReal = 0x20;
constexpr uint8_t kData = 0x40;
constexpr uint8_t kAscii = 0x50;
constexpr uint8_t kUtf16 = 0x60;
constexpr uint8_t kArray = 0xA0;
constexpr uint8_t kDict = 0xD0;
constexpr uint8_t kLongCount = 0x0F;

void putBig(std::string& out, uint64_t value, size_t bytes) {
    for (size_t i = bytes; i-- > 0;) out.push_back(char((value >> (8 * i)) & 0xFF));
}

void putInteger(std::string& out, int64_t value) {
    if (value >= 0 && value <= 0xFF) {
        out.push_back(char(kInteger));
        putBig(out, uint64_t(value), 1);
    } else if (value >= 0 && value <= 0xFFFF) {
        out.push_back(char(kInteger | 1));
        putBig(out, uint64_t(value), 2);
    } else if (value >= 0 && value <= int64_t{0xFFFFFFFF}) {
        out.push_back(char(kInteger | 2));
        putBig(out, uint64_t(value), 4);
    } else {
        out.push_back(char(kInteger | 3));
        putBig(out, uint64_t(value), 8);
    }
}

void putMarker(std::string& out, uint8_t kind, size_t count) {
    if (count < kLongCount) {
        out.push_back(char(kind | count));
        return;
    }
    out.push_back(char(kind | kLongCount));
    putInteger(out, int64_t(count));
}

size_t bytesFor(uint64_t largest) {
    return largest <= 0xFF ? 1 : largest <= 0xFFFF ? 2 : largest <= 0xFFFFFFFF ? 4 : 8;
}

size_t countOf(const Plist& p) {
    size_t count = 1 + (p.kind == Plist::Kind::Dict ? p.keys.size() : 0);
    for (const auto& item : p.items) count += countOf(item);
    return count;
}

class Writer {
public:
    explicit Writer(size_t count) : refBytes_(bytesFor(count)) {}

    size_t add(const Plist& p) {
        const size_t index = objects_.size();
        objects_.emplace_back();
        std::string body;
        switch (p.kind) {
        case Plist::Kind::Boolean:
            body.push_back(char(p.boolean ? kTrue : kFalse));
            break;
        case Plist::Kind::Integer:
            putInteger(body, p.integer);
            break;
        case Plist::Kind::Real:
            body.push_back(char(kReal | 3));
            putBig(body, std::bit_cast<uint64_t>(p.real), 8);
            break;
        case Plist::Kind::Data:
            putMarker(body, kData, p.text.size());
            body += p.text;
            break;
        case Plist::Kind::String:
            body = stringObject(p.text);
            break;
        case Plist::Kind::Array: {
            std::vector<size_t> refs;
            refs.reserve(p.items.size());
            for (const auto& item : p.items) refs.push_back(add(item));
            putMarker(body, kArray, refs.size());
            for (const size_t ref : refs) putBig(body, ref, refBytes_);
            break;
        }
        case Plist::Kind::Dict: {
            std::vector<size_t> keyRefs, valueRefs;
            for (const auto& key : p.keys) keyRefs.push_back(add(Plist::of(std::string_view(key))));
            for (const auto& value : p.items) valueRefs.push_back(add(value));
            putMarker(body, kDict, keyRefs.size());
            for (const size_t ref : keyRefs) putBig(body, ref, refBytes_);
            for (const size_t ref : valueRefs) putBig(body, ref, refBytes_);
            break;
        }
        }
        objects_[index] = std::move(body);
        return index;
    }

    std::string finish() const {
        std::string out(kMagic);
        std::vector<uint64_t> offsets;
        for (const auto& object : objects_) {
            offsets.push_back(out.size());
            out += object;
        }
        const uint64_t tableAt = out.size();
        const size_t offsetBytes = bytesFor(tableAt);
        for (const uint64_t offset : offsets) putBig(out, offset, offsetBytes);
        out.append(6, '\0');
        out.push_back(char(offsetBytes));
        out.push_back(char(refBytes_));
        putBig(out, objects_.size(), 8);
        putBig(out, 0, 8);
        putBig(out, tableAt, 8);
        return out;
    }

private:
    static std::string stringObject(const std::string& utf8) {
        std::string body;
        bool ascii = true;
        for (const char c : utf8) ascii = ascii && uint8_t(c) < 0x80;
        if (ascii) {
            putMarker(body, kAscii, utf8.size());
            return body + utf8;
        }
        const std::wstring wide = toWide(utf8);
        putMarker(body, kUtf16, wide.size());
        for (const wchar_t unit : wide) putBig(body, uint16_t(unit), 2);
        return body;
    }

    size_t refBytes_;
    std::vector<std::string> objects_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {
        if (bytes.size() < kMagic.size() + kTrailerBytes || !bytes.starts_with(kMagic.substr(0, 7)))
            throw std::runtime_error("not a binary plist");
        const size_t trailer = bytes.size() - kTrailerBytes;
        offsetBytes_ = size_t(big(trailer + 6, 1));
        refBytes_ = size_t(big(trailer + 7, 1));
        count_ = big(trailer + 8, 8);
        top_ = big(trailer + 16, 8);
        tableAt_ = big(trailer + 24, 8);
        if (offsetBytes_ < 1 || offsetBytes_ > 8 || refBytes_ < 1 || refBytes_ > 8 || count_ == 0 || count_ > kMaxObjects ||
            top_ >= count_ || tableAt_ > trailer || (trailer - tableAt_) / offsetBytes_ < count_)
            throw std::runtime_error("a damaged binary plist");
    }

    Plist root() { return object(top_, 0); }

private:
    uint64_t big(size_t at, size_t size) const {
        if (at > bytes_.size() || size > bytes_.size() - at) throw std::out_of_range("past the plist's end");
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) value = value << 8 | uint8_t(bytes_[at + i]);
        return value;
    }

    // A count kept in the marker's low bits, or in an integer object after it.
    uint64_t countAt(size_t& at, uint8_t low) const {
        if (low != kLongCount) return low;
        const auto marker = uint8_t(big(at, 1));
        if ((marker & 0xF0) != kInteger || (marker & 0x0F) > 3) throw std::runtime_error("a damaged count");
        const size_t size = size_t{1} << (marker & 0x0F);
        const uint64_t count = big(at + 1, size);
        at += 1 + size;
        return count;
    }

    std::string_view slice(size_t at, uint64_t size) const {
        if (at > bytes_.size() || size > bytes_.size() - at) throw std::out_of_range("past the plist's end");
        return bytes_.substr(at, size_t(size));
    }

    Plist object(uint64_t index, int depth) {
        if (index >= count_ || depth > kMaxDepth || ++visits_ > kMaxVisits) throw std::runtime_error("a damaged binary plist");
        auto at = size_t(big(size_t(tableAt_ + index * offsetBytes_), offsetBytes_));
        if (at >= tableAt_) throw std::runtime_error("a damaged binary plist");
        const auto marker = uint8_t(big(at++, 1));
        const auto low = uint8_t(marker & 0x0F);
        switch (marker & 0xF0) {
        case 0x00:
            if (marker == kFalse || marker == kTrue) return Plist::of(marker == kTrue);
            break;
        case kInteger: {
            if (low > 3) break;
            const size_t size = size_t{1} << low;
            const uint64_t value = big(at, size);
            return Plist::of(size == 8 ? std::bit_cast<int64_t>(value) : int64_t(value));
        }
        case kReal:
            if (low == 2) return Plist::of(double(std::bit_cast<float>(uint32_t(big(at, 4)))));
            if (low == 3) return Plist::of(std::bit_cast<double>(big(at, 8)));
            break;
        case kData: {
            const uint64_t size = countAt(at, low);
            return Plist::data(slice(at, size));
        }
        case kAscii: {
            const uint64_t size = countAt(at, low);
            return Plist::of(slice(at, size));
        }
        case kUtf16: {
            const uint64_t units = countAt(at, low);
            const std::string_view raw = slice(at, units * 2);
            std::wstring wide;
            for (size_t i = 0; i + 1 < raw.size(); i += 2) wide.push_back(wchar_t(uint8_t(raw[i]) << 8 | uint8_t(raw[i + 1])));
            return Plist::of(std::string_view(toUtf8(wide)));
        }
        case kArray: {
            const uint64_t size = countAt(at, low);
            slice(at, size * refBytes_);
            std::vector<Plist> items;
            items.reserve(size_t(size));
            for (uint64_t i = 0; i < size; ++i) items.push_back(object(big(size_t(at + i * refBytes_), refBytes_), depth + 1));
            return Plist::array(std::move(items));
        }
        case kDict: {
            const uint64_t size = countAt(at, low);
            slice(at, size * 2 * refBytes_);
            Plist dict = Plist::dict();
            for (uint64_t i = 0; i < size; ++i) {
                const Plist key = object(big(size_t(at + i * refBytes_), refBytes_), depth + 1);
                if (key.kind != Plist::Kind::String) throw std::runtime_error("a key that is not a string");
                dict.set(key.text, object(big(size_t(at + (size + i) * refBytes_), refBytes_), depth + 1));
            }
            return dict;
        }
        default:
            break;
        }
        throw std::runtime_error("a kind of object AirPlay does not send");
    }

    std::string_view bytes_;
    size_t offsetBytes_ = 0, refBytes_ = 0;
    uint64_t count_ = 0, top_ = 0, tableAt_ = 0, visits_ = 0;
};

} // namespace

Plist Plist::of(bool value) {
    Plist p;
    p.kind = Kind::Boolean;
    p.boolean = value;
    return p;
}

Plist Plist::of(int64_t value) {
    Plist p;
    p.kind = Kind::Integer;
    p.integer = value;
    return p;
}

Plist Plist::of(double value) {
    Plist p;
    p.kind = Kind::Real;
    p.real = value;
    return p;
}

Plist Plist::of(std::string_view text) {
    Plist p;
    p.kind = Kind::String;
    p.text = text;
    return p;
}

Plist Plist::data(std::string_view bytes) {
    Plist p;
    p.kind = Kind::Data;
    p.text = bytes;
    return p;
}

Plist Plist::array(std::vector<Plist> items) {
    Plist p;
    p.kind = Kind::Array;
    p.items = std::move(items);
    return p;
}

Plist Plist::dict() {
    return {};
}

Plist& Plist::set(std::string key, Plist value) {
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) {
            items[i] = std::move(value);
            return *this;
        }
    keys.push_back(std::move(key));
    items.push_back(std::move(value));
    return *this;
}

const Plist* Plist::find(std::string_view key) const {
    if (kind != Kind::Dict) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

std::string toBinaryPlist(const Plist& root) {
    Writer writer(countOf(root));
    writer.add(root);
    return writer.finish();
}

std::optional<Plist> parseBinaryPlist(std::string_view bytes) {
    try {
        return Reader(bytes).root();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace camel::server::airplay
