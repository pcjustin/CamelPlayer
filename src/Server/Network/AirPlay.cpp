// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <stdexcept>

#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::server::airplay {

namespace {

constexpr uint16_t kQuestionWantsUnicast = 0x8001; // class IN, answered to the asker's own port
constexpr uint16_t kFlagResponse = 0x8000;
constexpr int kMaxNameJumps = 32;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kMaxLabelBytes = 63;

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::vector<std::string> listIn(const TxtRecord& txt, const char* key) {
    std::vector<std::string> values;
    const auto found = txt.find(key);
    if (found == txt.end()) return values;
    std::string_view rest = found->second;
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        values.emplace_back(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return values;
}

bool lists(const TxtRecord& txt, const char* key, std::string_view value) {
    return std::ranges::count(listIn(txt, key), value) > 0;
}

bool lacks(const TxtRecord& txt, const char* key, std::string_view value) {
    return txt.contains(key) && !lists(txt, key, value);
}

std::string textOf(const TxtRecord& txt, const char* key) {
    const auto found = txt.find(key);
    return found == txt.end() ? std::string() : found->second;
}

void put8(std::string& out, int value) {
    out.push_back(char(uint8_t(value)));
}

void put16(std::string& out, uint16_t value) {
    put8(out, value >> 8);
    put8(out, value & 0xFF);
}

void put32(std::string& out, uint32_t value) {
    put16(out, uint16_t(value >> 16));
    put16(out, uint16_t(value & 0xFFFF));
}

class Reader {
public:
    explicit Reader(std::string_view message) : message_(message) {}

    size_t at() const { return at_; }
    void skip(size_t bytes) {
        need(bytes);
        at_ += bytes;
    }
    uint8_t byte() {
        need(1);
        return uint8_t(message_[at_++]);
    }
    uint16_t u16() {
        const uint16_t high = byte();
        return uint16_t(high << 8 | byte());
    }
    uint32_t u32() {
        const uint32_t high = u16();
        return high << 16 | u16();
    }
    std::string text(size_t bytes) {
        need(bytes);
        std::string out(message_.substr(at_, bytes));
        at_ += bytes;
        return out;
    }
    DnsName name() {
        DnsName labels;
        Reader from(message_);
        from.at_ = at_;
        bool jumped = false;
        int jumps = 0;
        for (uint8_t length = from.byte(); length != 0; length = from.byte()) {
            if ((length & 0xC0) == 0xC0) {
                const size_t target = size_t(length & 0x3F) << 8 | from.byte();
                if (!jumped) at_ = from.at_;
                jumped = true;
                if (++jumps > kMaxNameJumps) throw std::out_of_range("a name that points at itself");
                from.at_ = target;
                continue;
            }
            labels.push_back(from.text(length));
        }
        if (!jumped) at_ = from.at_;
        return labels;
    }

private:
    void need(size_t bytes) const {
        if (at_ + bytes > message_.size()) throw std::out_of_range("a message that ends early");
    }

    std::string_view message_;
    size_t at_ = 0;
};

TxtRecord readTxt(Reader& reader, size_t end) {
    TxtRecord txt;
    while (reader.at() < end) {
        const size_t length = reader.byte();
        const std::string entry = reader.text(length);
        const size_t equals = entry.find('=');
        if (equals != 0 && equals != std::string::npos) txt[lower(entry.substr(0, equals))] = entry.substr(equals + 1);
    }
    return txt;
}

std::optional<DnsRecord> readRecord(Reader& reader) {
    DnsRecord record;
    record.name = reader.name();
    const uint16_t type = reader.u16();
    reader.skip(6); // class and time to live
    const size_t length = reader.u16();
    const size_t end = reader.at() + length;
    bool known = true;
    if (type == uint16_t(DnsType::Ptr)) {
        record.type = DnsType::Ptr;
        record.target = reader.name();
    } else if (type == uint16_t(DnsType::Srv)) {
        record.type = DnsType::Srv;
        reader.skip(4); // priority and weight
        record.port = reader.u16();
        record.target = reader.name();
    } else if (type == uint16_t(DnsType::Txt)) {
        record.type = DnsType::Txt;
        record.txt = readTxt(reader, end);
    } else if (type == uint16_t(DnsType::A) && length == 4) {
        record.type = DnsType::A;
        const uint32_t address = reader.u32();
        record.address = std::format("{}.{}.{}.{}", address >> 24, address >> 16 & 0xFF, address >> 8 & 0xFF, address & 0xFF);
    } else {
        known = false;
    }
    if (reader.at() > end) throw std::out_of_range("a record longer than it says");
    reader.skip(end - reader.at());
    return known ? std::optional(std::move(record)) : std::nullopt;
}

} // namespace

DnsName serviceName(std::string_view service) {
    DnsName name;
    while (!service.empty()) {
        const size_t dot = service.find('.');
        name.emplace_back(service.substr(0, dot));
        service = dot == std::string_view::npos ? std::string_view{} : service.substr(dot + 1);
    }
    name.emplace_back("local");
    return name;
}

std::string queryMessage(const std::vector<DnsQuestion>& questions) {
    std::string out(kHeaderBytes, '\0');
    out[4] = char(questions.size() >> 8);
    out[5] = char(questions.size() & 0xFF);
    for (const auto& question : questions) {
        for (const auto& label : question.name) {
            out.push_back(char(std::min(label.size(), kMaxLabelBytes)));
            out.append(label, 0, kMaxLabelBytes);
        }
        out.push_back('\0');
        put16(out, uint16_t(question.type));
        put16(out, kQuestionWantsUnicast);
    }
    return out;
}

std::vector<DnsRecord> parseRecords(std::string_view message) {
    std::vector<DnsRecord> records;
    try {
        Reader reader(message);
        reader.skip(2);
        if (!(reader.u16() & kFlagResponse)) return records;
        const int questions = reader.u16();
        const int answers = reader.u16();
        const int authorities = reader.u16();
        const int additional = reader.u16();
        for (int i = 0; i < questions; ++i) {
            reader.name();
            reader.skip(4);
        }
        for (int i = 0; i < answers + authorities + additional; ++i)
            if (auto record = readRecord(reader)) records.push_back(std::move(*record));
    } catch (const std::out_of_range& e) {
        log::info("a damaged mDNS message: %s", e.what());
    }
    return records;
}

std::optional<std::pair<std::string, std::string>> parseRaopName(std::string_view label) {
    const size_t at = label.find('@');
    if (at == std::string_view::npos || at == 0) return std::nullopt;
    return std::pair(deviceIdFrom(label.substr(0, at)), validUtf8(label.substr(at + 1)));
}

std::string deviceIdFrom(std::string_view text) {
    std::string id;
    for (const unsigned char c : text)
        if (std::isxdigit(c)) id.push_back(char(std::toupper(c)));
    return id;
}

const char* supportId(Support support) {
    switch (support) {
    case Support::Supported:
        return "";
    case Support::EncryptedOnly:
        return "encrypted";
    case Support::Password:
        return "password";
    case Support::Format:
        return "format";
    case Support::Refused:
        return "refused";
    }
    return "";
}

Support supportFrom(const TxtRecord& raop) {
    if (!lists(raop, "et", "0")) return Support::EncryptedOnly;
    if (lower(textOf(raop, "pw")) == "true") return Support::Password;
    if (lacks(raop, "cn", "1") || lacks(raop, "sr", "44100") || lacks(raop, "ss", "16") || lacks(raop, "ch", "2"))
        return Support::Format;
    return Support::Supported;
}

bool isApple(const TxtRecord& raop) {
    const std::string model = textOf(raop, "am");
    return std::ranges::any_of(std::array<std::string_view, 5>{"AudioAccessory", "AppleTV", "AirPort", "Mac", "iMac"},
                               [&](std::string_view family) { return model.starts_with(family); });
}

std::vector<Receiver> receiversFrom(const std::vector<Announced>& announced) {
    std::map<std::string, const Announced*> leads;
    for (const auto& member : announced) {
        if (!isApple(member.raop)) continue;
        const std::string group = textOf(member.airplay, "tsid");
        const auto [lead, added] = leads.try_emplace(group.empty() ? member.deviceId : group, &member);
        if (!added && textOf(lead->second->airplay, "igl") != "1" &&
            (textOf(member.airplay, "igl") == "1" || member.deviceId < lead->second->deviceId))
            lead->second = &member;
    }
    std::vector<Receiver> receivers;
    for (const auto& [id, lead] : leads) {
        const std::string group = textOf(lead->airplay, "gpn");
        std::vector<Speaker> others;
        std::vector<std::string> speakers{lead->deviceId};
        for (const auto& member : announced)
            if (member.deviceId != lead->deviceId && textOf(member.airplay, "tsid") == id) {
                others.push_back({member.address, member.port});
                speakers.push_back(member.deviceId);
            }
        if (others.empty()) speakers.clear();
        std::ranges::sort(speakers);
        receivers.push_back({id, group.empty() ? lead->name : group, lead->address, lead->port, textOf(lead->raop, "vs"),
                             supportFrom(lead->raop), std::move(others), std::move(speakers)});
    }
    std::ranges::sort(receivers, {}, [](const Receiver& r) { return std::pair(r.name, r.id); });
    return receivers;
}

std::string alacFrame(const uint8_t* pcm) {
    constexpr size_t kHeaderBytes23Bits = 2;
    constexpr size_t kFrameBytes = 1412; // 23 header bits, 16 for each sample, 3 for the end tag
    std::string out(kFrameBytes, '\0');
    out[0] = 0x20; // a channel pair, instance 0
    out[2] = 0x02; // no size, no shift, not compressed; the first sample starts in this byte's last bit
    size_t at = kHeaderBytes23Bits;
    for (size_t i = 0; i < kFramesPerPacket * 2; ++i) {
        const auto sample = uint16_t(pcm[i * 2] | pcm[i * 2 + 1] << 8);
        out[at] = char(uint8_t(out[at]) | sample >> 15);
        out[at + 1] = char(sample >> 7 & 0xFF);
        out[at + 2] = char(sample << 1 & 0xFF);
        at += 2;
    }
    out[at] = char(uint8_t(out[at]) | 0x01); // the end tag, 111
    out[at + 1] = '\xC0';
    return out;
}

std::string rtpPacket(uint16_t sequence, uint32_t rtpTime, uint32_t ssrc, bool first, std::string_view alac) {
    constexpr uint8_t kAudioType = 0x60;
    constexpr uint8_t kMarker = 0x80;
    std::string out;
    out.reserve(12 + alac.size());
    put8(out, 0x80);
    put8(out, first ? kAudioType | kMarker : kAudioType);
    put16(out, sequence);
    put32(out, rtpTime);
    put32(out, ssrc);
    out += alac;
    return out;
}

std::string syncPacket(bool first, uint32_t playing, uint64_t ntpTime, uint32_t newest) {
    std::string out;
    put8(out, first ? 0x90 : 0x80);
    put8(out, 0xD4);
    put16(out, 7);
    put32(out, playing);
    put32(out, uint32_t(ntpTime >> 32));
    put32(out, uint32_t(ntpTime));
    put32(out, newest);
    return out;
}

std::string timingReply(std::string_view request, uint64_t received, uint64_t sent) {
    constexpr size_t kBytes = 32;
    constexpr size_t kSentAt = 24;
    if (request.size() != kBytes || (uint8_t(request[1]) & 0x7F) != 0x52) return {};
    std::string out;
    put8(out, 0x80);
    put8(out, 0xD3);
    put16(out, 7);
    put32(out, 0);
    out += request.substr(kSentAt, 8);
    put32(out, uint32_t(received >> 32));
    put32(out, uint32_t(received));
    put32(out, uint32_t(sent >> 32));
    put32(out, uint32_t(sent));
    return out;
}

std::optional<ResendRequest> parseResend(std::string_view message) {
    if (message.size() < 8 || (uint8_t(message[1]) & 0x7F) != 0x55) return std::nullopt;
    const auto u16 = [&](size_t at) { return uint16_t(uint8_t(message[at]) << 8 | uint8_t(message[at + 1])); };
    return ResendRequest{u16(4), u16(6)};
}

std::string resendPacket(uint16_t sequence, std::string_view rtp) {
    std::string out;
    put8(out, 0x80);
    put8(out, 0xD6);
    put16(out, sequence);
    out += rtp;
    return out;
}

std::string dmapTags(std::string_view title, std::string_view artist, std::string_view album) {
    std::string items;
    const auto add = [&](std::string_view name, std::string_view text) {
        items += name;
        put32(items, uint32_t(text.size()));
        items += text;
    };
    add("minm", title);
    add("asar", artist);
    add("asal", album);
    std::string out = "mlit";
    put32(out, uint32_t(items.size()));
    return out + items;
}

double volumeDb(uint32_t level, bool muted) {
    constexpr double kLowestDb = -30.0;
    return muted || level == 0 ? kMutedDb : kLowestDb * (1.0 - std::min(level, 100U) / 100.0);
}

std::string sessionDescription(std::string_view localAddress, std::string_view remoteAddress, std::string_view session) {
    return std::format("v=0\r\no=CamelPlayer {0} 0 IN IP4 {1}\r\ns=CamelPlayer\r\nc=IN IP4 {2}\r\nt=0 0\r\n"
                       "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
                       "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
                       session, localAddress, remoteAddress);
}

} // namespace camel::server::airplay
