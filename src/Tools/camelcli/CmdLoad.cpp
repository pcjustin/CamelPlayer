// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// load: simulated Control points for the remote targets (docs/Plan.md §17.8). Each speaks the Server API as a Control
// point does: pages the track list and the album wall, searches, fetches album covers and follows the zones' events,
// every call's time recorded. The first pairs with a code a paired device shows, the others with codes it asks for as
// a paired device; their tokens are kept in <out>\tokens.json for later runs.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Commands.h"
#include "Common/util/Text.h"
#include "Server/Remote/RemoteServer.h"

using namespace camel;
using namespace camel::server;
using nlohmann::json;
namespace fs = std::filesystem;

namespace camelcli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kPage = 100;       // the window's list pages
constexpr int64_t kSearchShown = 50; // the search page's first results

struct Record {
    int client = 0;
    std::string kind;
    double startMs = 0, ms = 0;
    bool ok = true;
    std::string detail;
};

class Recorder {
public:
    explicit Recorder(Clock::time_point start) : start_(start) {}
    void add(int client, std::string kind, Clock::time_point from, Clock::time_point to, bool ok, std::string detail = {}) {
        const std::scoped_lock lock(mutex_);
        records_.push_back({client, std::move(kind), msBetween(start_, from), msBetween(from, to), ok, std::move(detail)});
    }
    std::vector<Record> records() const {
        const std::scoped_lock lock(mutex_);
        return records_;
    }

private:
    static double msBetween(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    Clock::time_point start_;
    mutable std::mutex mutex_;
    std::vector<Record> records_;
};

template <class F> void timed(Recorder& recorder, int client, const char* kind, F&& call) {
    const auto from = Clock::now();
    try {
        call();
        recorder.add(client, kind, from, Clock::now(), true);
    } catch (const std::exception& e) {
        recorder.add(client, kind, from, Clock::now(), false, e.what());
    }
}

// What the zone being paused and played again should show, and when it was asked; each client records once.
struct Toggle {
    std::mutex mutex;
    std::string zone;
    std::optional<PlaybackState> wanted;
    Clock::time_point asked;
    std::vector<bool> seen;
};

std::optional<PlaybackState> stateOf(const Snapshot& s, const std::string& zone) {
    const auto found = std::ranges::find(s.zones, zone, &ZoneSnapshot::id);
    if (found == s.zones.end()) return std::nullopt;
    return found->playback.state;
}

// Words of the libraries gen-library writes and of common titles, whole and as the first letters typed.
std::vector<std::string> queries() {
    const std::array<const char*, 16> words{"Blue",  "Night", "River", "Echo", "Garden", "Silver", "Morning", "Song",
                                            "Light", "Ocean", "Dream", "月光", "夜曲",   "星空",   "Love",    "the"};
    std::vector<std::string> out;
    for (const std::string word : words) {
        out.push_back(word);
        if (word.size() > 3 && static_cast<unsigned char>(word[0]) < 0x80) out.push_back(word.substr(0, 3));
    }
    return out;
}

void browse(RemoteServer& server, int client, Clock::time_point end, Recorder& recorder) {
    std::mt19937 random(std::random_device{}() + unsigned(client));
    const auto words = queries();
    int64_t tracks = 0, albums = 0;
    std::vector<int64_t> listed, fetched;
    timed(recorder, client, "track page",
          [&] { tracks = server.browseTrackRows(TrackSort::Title, false, std::nullopt, 0, kPage, std::nullopt).total; });
    timed(recorder, client, "album page", [&] {
        const auto page = server.browseAlbums(AlbumSort::Title, std::nullopt, 0, kPage, std::nullopt);
        albums = page.total;
        for (const auto& album : page.albums) listed.push_back(album.id);
    });
    const auto pageAt = [&](int64_t total) {
        return total <= kPage ? int64_t{0} : std::uniform_int_distribution<int64_t>(0, (total - 1) / kPage)(random) * kPage;
    };
    while (Clock::now() < end) {
        const int choice = std::uniform_int_distribution<int>(0, 99)(random);
        if (choice < 35) {
            const auto sort = std::array{TrackSort::Title, TrackSort::Artist, TrackSort::Album}[size_t(choice % 3)];
            timed(recorder, client, "track page",
                  [&] { server.browseTrackRows(sort, false, std::nullopt, pageAt(tracks), kPage, std::nullopt); });
        } else if (choice < 60) {
            timed(recorder, client, "album page", [&] {
                for (const auto& album :
                     server.browseAlbums(AlbumSort::Title, std::nullopt, pageAt(albums), kPage, std::nullopt).albums)
                    listed.push_back(album.id);
            });
        } else if (choice < 80) {
            const auto& query = words[std::uniform_int_distribution<size_t>(0, words.size() - 1)(random)];
            timed(recorder, client, "search", [&] { server.searchLibrary(query, 0, kSearchShown, std::nullopt); });
        } else if (!fetched.empty() && choice < 88) {
            const auto album = fetched[std::uniform_int_distribution<size_t>(0, fetched.size() - 1)(random)];
            timed(recorder, client, "cover again", [&] { server.artwork({ArtworkKind::Album, album}); });
        } else if (!listed.empty()) {
            const auto at = std::uniform_int_distribution<size_t>(0, listed.size() - 1)(random);
            const auto album = listed[at];
            listed.erase(listed.begin() + std::ptrdiff_t(at));
            if (std::ranges::find(fetched, album) != fetched.end()) continue;
            timed(recorder, client, "cover first", [&] {
                if (!server.artwork({ArtworkKind::Album, album}))
                    throw std::runtime_error("no cover for album " + std::to_string(album));
            });
            fetched.push_back(album);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(100, 400)(random)));
    }
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::ranges::sort(values);
    return values[std::min(values.size() - 1, size_t(std::lround(p / 100.0 * double(values.size() - 1))))];
}

json readJson(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return json::object();
    return json::parse(in, nullptr, false);
}

} // namespace

// load --host <address> [--port 9440] [--code <code>] [--clients 9] [--seconds 300] [--out <dir>]
//      [--toggle <zone> [--every 10]]
int cmdLoad(const Args& args) {
    const auto host = option(args, L"--host");
    if (!host) {
        std::puts("usage: load --host <address> [--port 9440] [--code <code>] [--clients 9] [--seconds 300] [--out <dir>]\n"
                  "            [--toggle <zone> [--every 10]]");
        return 2;
    }
    const auto port = uint16_t(std::stoi(option(args, L"--port").value_or(L"9440")));
    const auto code = toUtf8(option(args, L"--code").value_or(L""));
    const int count = std::stoi(option(args, L"--clients").value_or(L"9"));
    const auto seconds = std::stoi(option(args, L"--seconds").value_or(L"300"));
    const fs::path out = option(args, L"--out").value_or(L"load");
    const auto zone = toUtf8(option(args, L"--toggle").value_or(L""));
    const auto every = std::chrono::seconds(std::stoi(option(args, L"--every").value_or(L"10")));
    fs::create_directories(out);
    auto tokens = readJson(out / L"tokens.json");
    if (!tokens.is_object()) tokens = json::object();

    std::vector<std::unique_ptr<RemoteServer>> clients;
    for (int i = 1; i <= count; ++i) {
        const auto name = "Load " + std::to_string(i);
        RemoteServer::Options options;
        options.host = toUtf8(*host);
        options.port = port;
        options.cache = out / L"cache" / toWide(name);
        std::error_code ignored;
        fs::remove_all(options.cache, ignored); // each run fetches its covers afresh before it fetches them again
        for (int attempt = 0;; ++attempt) {
            if (!tokens.contains(name)) {
                const auto given = clients.empty() ? code : clients.front()->pairingCode();
                try {
                    tokens[name] = RemoteServer::pair(options.host, port, name, given).token;
                } catch (const PairingRefused& refused) {
                    if (refused.code != "codeNeeded") throw;
                    std::fprintf(stderr, "%s is not paired: give --code, shown on a paired device\n", name.c_str());
                    return 1;
                }
                std::ofstream(out / L"tokens.json") << tokens.dump(2);
            }
            options.token = tokens[name].get<std::string>();
            try {
                clients.push_back(std::make_unique<RemoteServer>(options));
                break;
            } catch (const NotPaired&) {
                if (attempt > 0) throw;
                tokens.erase(name); // revoked since: paired again
            }
        }
        std::printf("%s connected\n", name.c_str());
    }

    const auto start = Clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    Recorder recorder(start);
    auto toggle = std::make_shared<Toggle>();
    toggle->zone = zone;
    toggle->seen.assign(clients.size(), true);
    for (size_t i = 0; i < clients.size(); ++i)
        clients[i]->setListener([toggle, &recorder, i](const Event& event) {
            const auto now = Clock::now();
            const std::scoped_lock lock(toggle->mutex);
            if (toggle->seen[i] || !toggle->wanted || stateOf(event.snapshot, toggle->zone) != toggle->wanted) return;
            toggle->seen[i] = true;
            recorder.add(int(i) + 1, *toggle->wanted == PlaybackState::Playing ? "play shown" : "pause shown", toggle->asked, now,
                         true);
        });

    std::vector<std::thread> threads;
    threads.reserve(clients.size() + 1);
    for (size_t i = 0; i < clients.size(); ++i) threads.emplace_back([&, i] { browse(*clients[i], int(i) + 1, end, recorder); });
    if (!zone.empty())
        threads.emplace_back([&] {
            while (Clock::now() + every < end) {
                std::this_thread::sleep_for(every);
                const auto state = stateOf(clients.front()->snapshot(), zone);
                Command command;
                command.kind = state == PlaybackState::Playing ? CommandKind::Pause : CommandKind::Play;
                command.zone = zone;
                {
                    const std::scoped_lock lock(toggle->mutex);
                    for (size_t i = 0; i < toggle->seen.size(); ++i)
                        if (!toggle->seen[i])
                            recorder.add(int(i) + 1, *toggle->wanted == PlaybackState::Playing ? "play shown" : "pause shown",
                                         toggle->asked, Clock::now(), false, "not shown");
                    toggle->wanted = state == PlaybackState::Playing ? PlaybackState::Paused : PlaybackState::Playing;
                    toggle->asked = Clock::now();
                    toggle->seen.assign(clients.size(), false);
                }
                try {
                    clients.front()->submit(command);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "toggle: %s\n", e.what());
                }
            }
        });
    for (auto& thread : threads) thread.join();
    for (auto& client : clients) client->setListener({});

    const auto records = recorder.records();
    std::ofstream csv(out / L"calls.csv");
    csv << "client,kind,startMs,ms,ok,detail\n";
    std::map<std::string, std::vector<double>> times;
    std::map<std::string, int> failures;
    for (const auto& r : records) {
        csv << r.client << ',' << r.kind << ',' << r.startMs << ',' << r.ms << ',' << (r.ok ? 1 : 0) << ",\"" << r.detail
            << "\"\n";
        auto& kind = times[r.kind];
        if (r.ok)
            kind.push_back(r.ms);
        else
            ++failures[r.kind];
    }
    json summary = {{"host", toUtf8(*host)}, {"clients", count}, {"seconds", seconds}, {"kinds", json::object()}};
    std::printf("%-12s %7s %6s %8s %8s %8s %8s\n", "kind", "calls", "failed", "p50 ms", "p95 ms", "p99 ms", "max ms");
    for (const auto& [kind, values] : times) {
        const double max = values.empty() ? 0 : *std::ranges::max_element(values);
        summary["kinds"][kind] = {{"calls", values.size()},        {"failed", failures[kind]},
                                  {"p50", percentile(values, 50)}, {"p95", percentile(values, 95)},
                                  {"p99", percentile(values, 99)}, {"max", max}};
        std::printf("%-12s %7zu %6d %8.1f %8.1f %8.1f %8.1f\n", kind.c_str(), values.size(), failures[kind],
                    percentile(values, 50), percentile(values, 95), percentile(values, 99), max);
    }
    std::ofstream(out / L"summary.json") << summary.dump(2);
    for (auto& client : clients) client->shutdown();
    return 0;
}

} // namespace camelcli
