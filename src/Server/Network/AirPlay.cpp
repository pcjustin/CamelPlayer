// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlay.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

#include "Common/util/Log.h"

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

void put16(std::string& out, uint16_t value) {
    out.push_back(char(value >> 8));
    out.push_back(char(value & 0xFF));
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
    return std::pair(deviceIdFrom(label.substr(0, at)), std::string(label.substr(at + 1)));
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

std::vector<Receiver> receiversFrom(const std::vector<Announced>& announced) {
    std::map<std::string, const Announced*> leads;
    for (const auto& member : announced) {
        const std::string group = textOf(member.airplay, "tsid");
        const auto [lead, added] = leads.try_emplace(group.empty() ? member.deviceId : group, &member);
        if (!added && textOf(lead->second->airplay, "igl") != "1" &&
            (textOf(member.airplay, "igl") == "1" || member.deviceId < lead->second->deviceId))
            lead->second = &member;
    }
    std::vector<Receiver> receivers;
    for (const auto& [id, lead] : leads) {
        const std::string group = textOf(lead->airplay, "gpn");
        receivers.push_back({id, group.empty() ? lead->name : group, lead->address, lead->port, textOf(lead->raop, "vs"),
                             supportFrom(lead->raop)});
    }
    std::ranges::sort(receivers, {}, [](const Receiver& r) { return std::pair(r.name, r.id); });
    return receivers;
}

std::string sessionDescription(std::string_view localAddress, std::string_view remoteAddress, std::string_view session) {
    return std::format("v=0\r\no=CamelPlayer {0} 0 IN IP4 {1}\r\ns=CamelPlayer\r\nc=IN IP4 {2}\r\nt=0 0\r\n"
                       "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
                       "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
                       session, localAddress, remoteAddress);
}

} // namespace camel::server::airplay
