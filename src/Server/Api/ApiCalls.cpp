// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Api/ApiCalls.h"

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>

#include "Server/Api/ApiJson.h"
#include "Server/Api/ApiTypesJson.h"

namespace camel::server::api {
namespace {

using nlohmann::json;
using Handler = std::function<json(IServer&, const json&, const CancellationToken&)>;

[[noreturn]] void invalid(const std::string& what) {
    throw std::invalid_argument("API: " + what);
}

template <class T> T arg(const json& args, size_t i) {
    if (!args.is_array() || i >= args.size()) invalid("argument " + std::to_string(i) + " is missing");
    return args[i].get<T>();
}

using Revision = std::optional<uint64_t>;
using Filter = std::optional<LibraryGroupFilter>;

// The calls carried as JSON; covers, artist pictures, uploads, exports and the diagnostic bundle go over HTTP.
const std::map<std::string, Handler, std::less<>>& calls() {
    static const std::map<std::string, Handler, std::less<>> table = {
        {"submit", [](IServer& s, const json& a, const auto&) { return toJson(s.submit(commandFromJson(arg<json>(a, 0)))); }},
        {"snapshot", [](IServer& s, const json&, const auto&) { return toJson(s.snapshot()); }},
        {"waitIdle",
         [](IServer& s, const json&, const auto&) {
             s.waitIdle();
             return json();
         }},
        {"browseTracks",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.browseTracks(arg<int64_t>(a, 0), arg<int64_t>(a, 1), arg<Revision>(a, 2), t));
         }},
        {"searchTracks",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.searchTracks(arg<std::string>(a, 0), arg<int64_t>(a, 1), arg<int64_t>(a, 2), arg<Revision>(a, 3), t));
         }},
        {"searchLibrary",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.searchLibrary(arg<std::string>(a, 0), arg<int64_t>(a, 1), arg<int64_t>(a, 2), arg<Revision>(a, 3), t,
                                         arg<bool>(a, 4)));
         }},
        {"playStatistics", [](IServer& s, const json& a,
                              const CancellationToken& t) { return json(s.playStatistics(arg<PlayStatisticsQuery>(a, 0), t)); }},
        {"rereadLibrary",
         [](IServer& s, const json&, const auto&) {
             s.rereadLibrary();
             return json();
         }},
        {"scanLibraryNow",
         [](IServer& s, const json& a, const auto&) { return json(s.scanLibraryNow(arg<std::optional<int64_t>>(a, 0))); }},
        {"clearArtworkCache",
         [](IServer& s, const json&, const auto&) {
             s.clearArtworkCache();
             return json();
         }},
        {"recentlyPlayedAlbums",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.recentlyPlayedAlbums(arg<int64_t>(a, 0), arg<int64_t>(a, 1), t));
         }},
        {"browseAlbums",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.browseAlbums(arg<AlbumSort>(a, 0), arg<Filter>(a, 1), arg<int64_t>(a, 2), arg<int64_t>(a, 3),
                                        arg<Revision>(a, 4), t));
         }},
        {"browseGroups",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.browseGroups(arg<LibraryGroupField>(a, 0), arg<bool>(a, 1), arg<int64_t>(a, 2), arg<int64_t>(a, 3),
                                        arg<Revision>(a, 4), t));
         }},
        {"groupInitials", [](IServer& s, const json& a,
                             const CancellationToken& t) { return json(s.groupInitials(arg<LibraryGroupField>(a, 0), t)); }},
        {"albumInitials",
         [](IServer& s, const json& a, const CancellationToken& t) { return json(s.albumInitials(arg<AlbumSort>(a, 0), t)); }},
        {"creditParts", [](IServer& s, const json& a,
                           const CancellationToken& t) { return json(s.creditParts(arg<std::vector<std::string>>(a, 0), t)); }},
        {"browseTrackRows",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.browseTrackRows(arg<TrackSort>(a, 0), arg<bool>(a, 1), arg<Filter>(a, 2), arg<int64_t>(a, 3),
                                           arg<int64_t>(a, 4), arg<Revision>(a, 5), t));
         }},
        {"albumPosition",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.albumPosition(arg<AlbumSort>(a, 0), arg<Filter>(a, 1), arg<bool>(a, 2), arg<int64_t>(a, 3), t));
         }},
        {"trackRowPosition",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.trackRowPosition(arg<TrackSort>(a, 0), arg<bool>(a, 1), arg<Filter>(a, 2), arg<int64_t>(a, 3), t));
         }},
        {"artistPage", [](IServer& s, const json& a,
                          const CancellationToken& t) { return json(s.artistPage(arg<LibraryGroupFilter>(a, 0), t)); }},
        {"smartTracks",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.smartTracks(arg<SmartList>(a, 0), arg<int64_t>(a, 1), arg<int64_t>(a, 2), arg<Revision>(a, 3), t));
         }},
        {"favoriteAlbums",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.favoriteAlbums(arg<int64_t>(a, 0), arg<int64_t>(a, 1), arg<Revision>(a, 2), t));
         }},
        {"favoriteArtists",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.favoriteArtists(arg<int64_t>(a, 0), arg<int64_t>(a, 1), arg<Revision>(a, 2), t));
         }},
        {"artistPicture",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.artistPicture(arg<std::string>(a, 0), arg<bool>(a, 1), t));
         }},
        {"removeArtistPicture",
         [](IServer& s, const json& a, const auto&) { return json(s.removeArtistPicture(arg<std::string>(a, 0))); }},
        {"chooseArtistCandidate",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.chooseArtistCandidate(arg<std::string>(a, 0), arg<std::string>(a, 1), t));
         }},
        {"albumDetail",
         [](IServer& s, const json& a, const CancellationToken& t) { return json(s.albumDetail(arg<int64_t>(a, 0), t)); }},
        {"albumTrackPaths",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.albumTrackPaths(arg<std::vector<int64_t>>(a, 0), t));
         }},
        {"browseFolder", [](IServer& s, const json& a,
                            const CancellationToken& t) { return json(s.browseFolder(arg<std::optional<int64_t>>(a, 0), t)); }},
        {"folderTracks",
         [](IServer& s, const json& a, const CancellationToken& t) { return json(s.folderTracks(arg<int64_t>(a, 0), t)); }},
        {"folderTracksFrom",
         [](IServer& s, const json& a, const CancellationToken& t) { return json(s.folderTracksFrom(arg<int64_t>(a, 0), t)); }},
        {"fileFolders", [](IServer& s, const json& a, const auto&) { return json(s.fileFolders(arg<int64_t>(a, 0))); }},
        {"libraryRoots", [](IServer& s, const json&, const auto&) { return json(s.libraryRoots()); }},
        {"serverFolders", [](IServer& s, const json& a, const auto&) { return json(s.serverFolders(arg<std::string>(a, 0))); }},
        {"playerFirewallRule", [](IServer& s, const json&, const auto&) { return json(s.playerFirewallRule()); }},
        {"libraryRootContents",
         [](IServer& s, const json& a, const auto&) { return json(s.libraryRootContents(arg<int64_t>(a, 0))); }},
        {"libraryScanProgress", [](IServer& s, const json&, const auto&) { return json(s.libraryScanProgress()); }},
        {"loudnessProgress", [](IServer& s, const json&, const auto&) { return json(s.loudnessProgress()); }},
        {"playlists", [](IServer& s, const json&, const auto&) { return json(s.playlists()); }},
        {"playlistItems",
         [](IServer& s, const json& a, const auto&) {
             return json(s.playlistItems(arg<int64_t>(a, 0), arg<int64_t>(a, 1), arg<int64_t>(a, 2)));
         }},
        {"previewSelection",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.previewSelection(arg<std::vector<PlaylistSource>>(a, 0), t));
         }},
        {"createPlaylist",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.createPlaylist(arg<std::string>(a, 0), arg<std::vector<PlaylistSource>>(a, 1), t));
         }},
        {"addToPlaylist",
         [](IServer& s, const json& a, const CancellationToken& t) {
             return json(s.addToPlaylist(arg<int64_t>(a, 0), arg<std::optional<int64_t>>(a, 1),
                                         arg<std::vector<PlaylistSource>>(a, 2), t));
         }},
        {"saveQueueAsPlaylist",
         [](IServer& s, const json& a, const auto&) {
             return json(s.saveQueueAsPlaylist(arg<std::string>(a, 0), arg<std::string>(a, 1)));
         }},
        {"editPlaylist", [](IServer& s, const json& a, const auto&) { return json(s.editPlaylist(arg<PlaylistEdit>(a, 0))); }},
        {"undoPlaylistChange",
         [](IServer& s, const json& a, const auto&) { return json(s.undoPlaylistChange(arg<PlaylistUndo>(a, 0))); }},
        {"dataRecovery", [](IServer& s, const json&, const auto&) { return json(s.dataRecovery()); }},
        {"discordStatus", [](IServer& s, const json&, const auto&) { return json(s.discordStatus()); }},
        {"lyrics", [](IServer& s, const json& a, const auto&) { return json(s.lyrics(arg<std::string>(a, 0))); }},
        {"fileDetails", [](IServer& s, const json& a, const auto&) { return json(s.fileDetails(arg<std::string>(a, 0))); }},
        {"waveform", [](IServer& s, const json& a, const auto&) { return json(s.waveform(arg<std::string>(a, 0))); }},
        {"setDiscordTexts",
         [](IServer& s, const json& a, const auto&) {
             s.setDiscordTexts(arg<DiscordTexts>(a, 0));
             return json();
         }},
    };
    return table;
}

// Each queue of a snapshot's JSON, keyed by its zone, the snapshot's own (the selected zone's, when it is sent apart)
// by a key no zone has.
template <class F> void eachQueue(json& snapshot, F visit) {
    if (snapshot.is_object() && snapshot.contains("zones") && snapshot["zones"].is_array())
        for (auto& zone : snapshot["zones"])
            if (zone.is_object() && zone.contains("queue") && zone.contains("id") && zone["id"].is_string())
                visit("zone:" + zone["id"].get<std::string>(), zone["queue"]);
    if (snapshot.is_object() && snapshot.contains("queue")) visit(std::string("selected"), snapshot["queue"]);
}

} // namespace

json answer(IServer& server, const std::string& call, const json& args, const CancellationToken& token) {
    const auto found = calls().find(call);
    if (found == calls().end()) invalid("no call " + call);
    try {
        return found->second(server, args, token);
    } catch (const json::exception& e) {
        invalid(call + ": " + e.what());
    }
}

json errorOf(const std::exception_ptr& error) {
    const auto as = [](const char* code, const char* message) { return json{{"code", code}, {"message", message}}; };
    try {
        std::rethrow_exception(error);
    } catch (const LibraryRevisionChanged& e) {
        return as("revisionChanged", e.what());
    } catch (const json::exception& e) {
        return as("invalidArgument", e.what());
    } catch (const std::invalid_argument& e) {
        return as("invalidArgument", e.what());
    } catch (const std::logic_error& e) {
        return as("notAllowed", e.what());
    } catch (const std::exception& e) {
        return as("failed", e.what());
    } catch (...) {
        return as("failed", "an unknown error");
    }
}

void rethrow(const json& error) {
    const auto text = [&error](const char* key) {
        return error.is_object() && error.contains(key) && error[key].is_string() ? error[key].get<std::string>() : std::string();
    };
    const std::string code = text("code");
    const std::string message = text("message").empty() ? code : text("message");
    if (code == "revisionChanged") throw LibraryRevisionChanged();
    if (code == "invalidArgument") throw std::invalid_argument(message);
    if (code == "notAllowed") throw std::logic_error(message);
    throw std::runtime_error(message);
}

void QueueEncoder::encode(json& snapshot) {
    eachQueue(snapshot, [this](const std::string& key, json& queue) {
        if (!queue.is_object() || !queue.contains("entries") || !queue["entries"].is_array()) return;
        const json entries = queue["entries"];
        auto& sent = sent_[key];
        if (sent.is_array()) {
            const size_t most = std::min(sent.size(), entries.size());
            size_t prefix = 0, suffix = 0;
            while (prefix < most && sent[prefix] == entries[prefix]) ++prefix;
            while (suffix < most - prefix && sent[sent.size() - 1 - suffix] == entries[entries.size() - 1 - suffix]) ++suffix;
            if (prefix == entries.size() && sent.size() == entries.size()) {
                queue.erase("entries");
                queue["same"] = true;
            } else {
                queue["keep"] = {prefix, suffix};
                queue["entries"] = json(entries.begin() + std::ptrdiff_t(prefix), entries.end() - std::ptrdiff_t(suffix));
            }
        }
        sent = entries;
    });
}

void QueueDecoder::decode(json& snapshot) {
    eachQueue(snapshot, [this](const std::string& key, json& queue) {
        if (!queue.is_object()) invalid("a queue that is not an object");
        auto& held = held_[key];
        if (queue.contains("same")) {
            if (!held.is_array()) invalid("a queue said to be unchanged before any was sent");
            queue.erase("same");
            queue["entries"] = held;
            return;
        }
        if (!queue.contains("entries") || !queue["entries"].is_array()) invalid("a queue without entries");
        if (queue.contains("keep")) {
            const json& keep = queue["keep"];
            if (!held.is_array() || !keep.is_array() || keep.size() != 2 || !keep[0].is_number_unsigned() ||
                !keep[1].is_number_unsigned())
                invalid("a queue change without what it keeps");
            const auto prefix = keep[0].get<uint64_t>(), suffix = keep[1].get<uint64_t>();
            if (prefix > held.size() || suffix > held.size() - prefix) invalid("a queue change keeping more than there is");
            json full(held.begin(), held.begin() + std::ptrdiff_t(prefix));
            for (const auto& entry : queue["entries"]) full.push_back(entry);
            for (auto at = held.end() - std::ptrdiff_t(suffix); at != held.end(); ++at) full.push_back(*at);
            queue.erase("keep");
            queue["entries"] = std::move(full);
        }
        held = queue["entries"];
    });
}

} // namespace camel::server::api
