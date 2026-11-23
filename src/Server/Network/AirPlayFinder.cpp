// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlayFinder.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Network/LocalAddresses.h"
#include "Server/Network/Rtsp.h"
#include "Server/Network/SocketWait.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using namespace airplay;
using Clock = std::chrono::steady_clock;

constexpr auto kPollSockets = std::chrono::milliseconds(100);
constexpr int kQueriesInASearch = 3;
constexpr int kSilentSearches = 2; // a service that answered no query for this many searches has gone
constexpr DWORD kMulticastTtl = 255;
constexpr SOCKET kNoSocket = INVALID_SOCKET;
constexpr std::string_view kRaopService = "_raop._tcp";
constexpr std::string_view kAirPlayService = "_airplay._tcp";
constexpr int kStatusOk = 200;
constexpr int kStatusUnauthorized = 401;
constexpr int kStatusForbidden = 403;

std::string keyOf(const DnsName& name) {
    std::string key;
    for (const auto& label : name) {
        key += label;
        key += '\0';
    }
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return key;
}

bool under(const DnsName& instance, const DnsName& service) {
    return instance.size() == service.size() + 1 && keyOf(DnsName(instance.begin() + 1, instance.end())) == keyOf(service);
}

// What the status of a request says about the receiver; other answers, such as being busy, say nothing.
std::optional<Support> verdictOf(int status) {
    if (status == kStatusForbidden) return Support::Refused;
    if (status == kStatusUnauthorized) return Support::Password;
    if (status == kStatusOk) return Support::Supported;
    return std::nullopt;
}

struct Answer {
    Support support = Support::Supported;
    bool transient = false;
};

struct Waits {
    int request = 0, user = 0; // for ANNOUNCE, which a Mac answers once its user accepts CamelPlayer
};

// What OPTIONS says and, for a session, what ANNOUNCE says after it.
std::optional<Support> askOn(RtspClient& client, const Receiver& receiver, bool session, Waits waits, bool& optionsRefused) {
    const auto options = verdictOf(client.request("OPTIONS", "*").status);
    optionsRefused = options == Support::Refused;
    // ANNOUNCE is AirPlay 1's: one played over AirPlay 2 is asked for a session when it plays.
    if (!session || options != Support::Supported || receiver.airplay2) return options;
    const auto description = sessionDescription(client.localAddress(), receiver.address, client.session());
    client.setTimeout(waits.user);
    const auto announced = verdictOf(client.request("ANNOUNCE", client.url(), "application/sdp", description).status);
    client.setTimeout(waits.request);
    if (announced == Support::Supported) client.request("TEARDOWN", client.url());
    return announced;
}

// What a receiver that does not take an unpaired sender needs from the user.
Support pairingWanted(const Receiver& receiver) {
    return receiver.asksPassword ? Support::Password : receiver.asksCode ? Support::Code : Support::Refused;
}

std::optional<Answer> ask(const Receiver& receiver, bool session, Waits waits, AirPlayPairings* pairings) {
    try {
        bool optionsRefused = false;
        if (pairings && pairings->find(receiver.id)) {
            RtspClient client(receiver.address, receiver.port, waits.request);
            // One that no longer takes what it was paired with asks for it again.
            if (pairFor(client, receiver, pairings) != PairOutcome::Paired) return Answer{pairingWanted(receiver)};
            const auto verdict = askOn(client, receiver, session, waits, optionsRefused);
            return verdict ? std::optional(Answer{*verdict}) : std::nullopt;
        }
        {
            RtspClient client(receiver.address, receiver.port, waits.request);
            const auto plain = askOn(client, receiver, session, waits, optionsRefused);
            if (!optionsRefused) return plain ? std::optional(Answer{*plain}) : std::nullopt;
        }
        // One that refuses even OPTIONS may take a session once paired.
        RtspClient paired(receiver.address, receiver.port, waits.request);
        if (paired.pairTransiently() != PairOutcome::Paired) return Answer{pairingWanted(receiver)};
        const auto after = askOn(paired, receiver, session, waits, optionsRefused);
        if (!after) return std::nullopt;
        return Answer{*after, *after == Support::Supported};
    } catch (const std::exception& e) {
        log::info("AirPlay receiver at %s: %s", receiver.address.c_str(), e.what());
    }
    return std::nullopt;
}

// What a verdict holds for: a receiver that turns its password or code on or off is asked again.
std::string askedAt(const Receiver& receiver) {
    return std::format("{}:{} {} password={} code={}", receiver.address, receiver.port, receiver.version, receiver.asksPassword,
                       receiver.asksCode);
}

} // namespace

struct AirPlayFinder::Impl {
    struct Service {
        DnsName name;
        uint16_t port = 0;
        std::string host;
        TxtRecord txt;
        Clock::time_point seen;
    };
    struct Verdict {
        std::string asked; // the address and version it holds for
        Support support = Support::Supported;
        bool transient = false;
    };

    WinsockScope winsock;
    AirPlayFinderOptions options;
    std::function<void()> onChange;
    SOCKET udp = INVALID_SOCKET;
    SOCKET group = kNoSocket; // on the group's port, shared with Windows' own responder
    DnsName raopService = serviceName(kRaopService);
    DnsName airplayService = serviceName(kAirPlayService);
    // A host on several networks answers from each with its own address: the one in use stays while the host gave it in
    // this search or the last one.
    struct Host {
        std::string address;
        std::set<std::string> thisSearch, lastSearch;
    };
    // Only the finder's thread.
    std::map<std::string, Service> services; // by name
    std::map<std::string, Host> hosts;       // by host name
    // Only the asking thread.
    std::map<std::string, std::string> lastAsked; // by receiver
    std::map<std::string, uint64_t> changesAsked; // by receiver, `settingChanges` as it was last asked
    mutable std::mutex mutex;
    std::map<std::string, Verdict> verdicts; // by receiver
    // By receiver, each other setting it announced, even one it went back to before the asking thread looked again.
    std::map<std::string, uint64_t> settingChanges;
    std::atomic<bool> verdictsChanged = false;
    std::vector<Receiver> known;
    std::set<std::string> toCheck;
    std::set<std::string> checking; // asked for a session now, or about to be
    std::set<std::string> pairedNow;
    std::atomic<bool> stopping = false, searchWanted = false;
    std::thread thread;
    // Receivers are asked on a thread of their own: a Mac holds its answer for up to 20 s while its user decides, and the
    // search goes on meanwhile.
    std::thread asker;

    Impl(AirPlayFinderOptions finderOptions, std::function<void()> changed)
        : options(std::move(finderOptions)), onChange(std::move(changed)) {
        udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            log::warn("AirPlay receivers: cannot search (%d)", WSAGetLastError());
        setsockopt(udp, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&kMulticastTtl), sizeof(kMulticastTtl));
        if (options.groupPort) joinGroup();
        thread = std::thread([this] { run(); });
        asker = std::thread([this] {
            while (!stopping) {
                askReceivers();
                std::this_thread::sleep_for(kPollSockets);
            }
        });
    }

    ~Impl() {
        stopping = true;
        thread.join();
        asker.join();
        closesocket(udp);
        if (group != kNoSocket) closesocket(group);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // A responder need not send a service's details with its name, so what is still missing is asked for by name.
    std::vector<DnsQuestion> questions() const {
        std::vector<DnsQuestion> list = {{raopService, DnsType::Ptr}, {airplayService, DnsType::Ptr}};
        for (const auto& [key, service] : services) {
            if (service.host.empty()) list.push_back({service.name, DnsType::Srv});
            if (service.txt.empty()) list.push_back({service.name, DnsType::Txt});
        }
        return list;
    }

    // Joins the group on every interface, the loopback one included, for answers and announcements sent to it.
    void joinGroup() {
        group = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const BOOL reuse = TRUE;
        setsockopt(group, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options.groupPort);
        if (bind(group, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            log::warn("AirPlay receivers: cannot listen on the mDNS group's port (%d)", WSAGetLastError());
            closesocket(group);
            group = kNoSocket;
            return;
        }
        setsockopt(group, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&kMulticastTtl), sizeof(kMulticastTtl));
        auto interfaces = localAddresses();
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        interfaces.push_back(loopback);
        for (const auto& local : interfaces) {
            ip_mreq membership{};
            inet_pton(AF_INET, options.groupHost.c_str(), &membership.imr_multiaddr);
            membership.imr_interface = local;
            setsockopt(group, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership));
        }
    }

    // The same questions from the group's port, to the group, answered to the group.
    void sendGroupQuery() {
        if (group == kNoSocket) return;
        const std::string message = queryMessage(questions(), false);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(options.groupPort);
        inet_pton(AF_INET, options.groupHost.c_str(), &target.sin_addr);
        auto interfaces = localAddresses();
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        interfaces.push_back(loopback);
        for (const auto& local : interfaces) {
            setsockopt(group, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local));
            sendto(group, message.data(), int(message.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        }
    }

    void sendQuery() {
        sendGroupQuery();
        const std::string message = queryMessage(questions());
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(options.queryPort);
        inet_pton(AF_INET, options.queryHost.c_str(), &target.sin_addr);
        const auto send = [&] {
            sendto(udp, message.data(), int(message.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        };
        if ((ntohl(target.sin_addr.s_addr) >> 28) != 0xE) { // a test's unicast address
            send();
            return;
        }
        for (const auto& local : localAddresses()) {
            setsockopt(udp, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local));
            send();
        }
    }

    Service* serviceFor(const DnsName& name) {
        if (!under(name, raopService) && !under(name, airplayService)) return nullptr;
        Service& service = services[keyOf(name)];
        service.name = name;
        service.seen = Clock::now();
        return &service;
    }

    void apply(const std::vector<DnsRecord>& records) {
        for (const auto& record : records) {
            if (record.type == DnsType::Ptr) {
                serviceFor(record.target);
            } else if (record.type == DnsType::Srv) {
                if (auto* service = serviceFor(record.name)) {
                    service->port = record.port;
                    service->host = keyOf(record.target);
                }
            } else if (record.type == DnsType::Txt) {
                if (auto* service = serviceFor(record.name)) service->txt = record.txt;
            } else if (!record.address.starts_with("169.254.")) {
                auto& host = hosts[keyOf(record.name)];
                host.thisSearch.insert(record.address);
                if (host.address.empty() || (!host.thisSearch.contains(host.address) && !host.lastSearch.contains(host.address)))
                    host.address = record.address;
            }
        }
    }

    // Each receiver by its RAOP service, with its _airplay._tcp record, and those that announce _airplay._tcp alone.
    std::vector<Announced> announced() const {
        std::map<std::string, const Service*> airplayServices;
        for (const auto& [key, service] : services)
            if (const auto id = service.txt.find("deviceid"); under(service.name, airplayService) && id != service.txt.end())
                airplayServices[deviceIdFrom(id->second)] = &service;
        std::vector<Announced> found;
        std::set<std::string> withRaop;
        for (const auto& [key, service] : services) {
            const auto host = hosts.find(service.host);
            const auto name = parseRaopName(service.name.front());
            if (!under(service.name, raopService) || !name || !service.port || service.txt.empty() || host == hosts.end())
                continue;
            Announced receiver{name->first, name->second, host->second.address, service.port, service.txt, {}};
            if (const auto airplay = airplayServices.find(name->first); airplay != airplayServices.end())
                receiver.airplay = airplay->second->txt;
            withRaop.insert(name->first);
            found.push_back(std::move(receiver));
        }
        for (const auto& [id, service] : airplayServices) {
            const auto host = hosts.find(service->host);
            if (withRaop.contains(id) || !service->port || host == hosts.end()) continue;
            found.push_back({id, service->name.front(), host->second.address, service->port, {}, service->txt});
        }
        return found;
    }

    void update() {
        verdictsChanged = false;
        std::erase_if(services, [&](const auto& entry) {
            return Clock::now() - entry.second.seen > kSilentSearches * options.searchInterval;
        });
        auto receivers = receiversFrom(announced());
        {
            std::scoped_lock lock(mutex);
            for (auto& receiver : receivers) {
                const auto before = std::ranges::find(known, receiver.id, &Receiver::id);
                if (before != known.end() && askedAt(*before) != askedAt(receiver)) ++settingChanges[receiver.id];
                const auto verdict = verdicts.find(receiver.id);
                if (verdict != verdicts.end() && verdict->second.asked == askedAt(receiver) &&
                    (receiver.support == Support::Supported || receiver.support == Support::Password)) {
                    receiver.support = verdict->second.support;
                    receiver.transient = verdict->second.transient;
                }
            }
            if (receivers == known) return;
            known = std::move(receivers);
        }
        if (onChange) onChange();
    }

    void judge(const Receiver& receiver, bool session) {
        const auto verdict = ask(receiver, session, {options.sessionTimeoutMs, options.userTimeoutMs}, options.pairings.get());
        if (!verdict) return;
        std::scoped_lock lock(mutex);
        verdicts[receiver.id] = {askedAt(receiver), verdict->support, verdict->transient};
        verdictsChanged = true;
    }

    void askReceivers() {
        std::vector<Receiver> receivers;
        std::set<std::string> wanted, paired;
        std::map<std::string, uint64_t> changes;
        {
            std::scoped_lock lock(mutex);
            receivers = known;
            changes = settingChanges;
            wanted.swap(toCheck);
            paired.swap(pairedNow);
            for (const auto& receiver : receivers)
                if (paired.contains(receiver.id))
                    verdicts[receiver.id] = {askedAt(receiver), Support::Supported, receiver.transient};
        }
        if (!paired.empty()) verdictsChanged = true;
        for (const auto& receiver : receivers) {
            if (stopping) return;
            const bool session = wanted.contains(receiver.id);
            // What it answered before is asked again when the user asks, as its own setting may have changed since, and
            // once it announces another setting, even one it went back to.
            const bool keptPairing = options.pairings && options.pairings->find(receiver.id);
            const bool judged = [&] {
                std::scoped_lock lock(mutex);
                return verdicts.contains(receiver.id);
            }();
            const bool askable = receiver.support == Support::Supported || keptPairing || (session && judged);
            std::string& last = lastAsked[receiver.id];
            uint64_t& changesBefore = changesAsked[receiver.id];
            const bool unchanged = last == askedAt(receiver) && changesBefore == changes[receiver.id];
            last = askedAt(receiver);
            changesBefore = changes[receiver.id];
            if (askable && (session || !unchanged)) judge(receiver, session);
            if (session) {
                std::scoped_lock lock(mutex);
                checking.erase(receiver.id);
            }
        }
        std::scoped_lock lock(mutex);
        for (const auto& id : wanted)
            if (!toCheck.contains(id)) checking.erase(id); // not found, so not asked
    }

    void run() {
        auto nextQuery = Clock::now();
        int queriesLeft = kQueriesInASearch;
        while (!stopping) {
            if (searchWanted.exchange(false)) {
                nextQuery = Clock::now();
                queriesLeft = kQueriesInASearch;
            }
            if (Clock::now() >= nextQuery) {
                if (queriesLeft == kQueriesInASearch)
                    for (auto& [name, host] : hosts) host.lastSearch = std::exchange(host.thisSearch, {});
                sendQuery();
                nextQuery = Clock::now() + (--queriesLeft > 0 ? options.retryInterval : options.searchInterval);
                if (queriesLeft == 0) queriesLeft = kQueriesInASearch;
                update();
            }
            if (verdictsChanged) update();
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(udp, &readable);
            if (group != kNoSocket) FD_SET(group, &readable);
            if (!waitReadable(readable, kPollSockets)) continue;
            for (const SOCKET from : {udp, group}) {
                if (from == kNoSocket || !FD_ISSET(from, &readable)) continue;
                char buffer[9000];
                const int length = recv(from, buffer, sizeof(buffer), 0);
                if (length <= 0) continue;
                apply(parseRecords(std::string_view(buffer, size_t(length))));
            }
            update();
        }
    }
};

AirPlayFinder::AirPlayFinder(AirPlayFinderOptions options, std::function<void()> onChange)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(onChange))) {}

AirPlayFinder::~AirPlayFinder() = default;

std::vector<airplay::Receiver> AirPlayFinder::receivers() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->known;
}

void AirPlayFinder::search() {
    impl_->searchWanted = true;
}

void AirPlayFinder::check(const std::string& id) {
    std::scoped_lock lock(impl_->mutex);
    impl_->toCheck.insert(id);
    impl_->checking.insert(id);
}

void AirPlayFinder::paired(const std::string& id) {
    std::scoped_lock lock(impl_->mutex);
    impl_->pairedNow.insert(id);
    impl_->toCheck.insert(id);
    impl_->checking.insert(id);
}

bool AirPlayFinder::checking(const std::string& id) const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->checking.contains(id);
}

} // namespace camel::server
