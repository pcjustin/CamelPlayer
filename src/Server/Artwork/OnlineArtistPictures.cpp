// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Artwork/OnlineArtistPictures.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>

#include "Common/util/Text.h"
#include "Server/Support/Http.h"

namespace camel::server::online {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxJsonBytes = 4u << 20;
constexpr size_t kMaxImageBytes = 16u << 20;

std::string text(const json& j, const char* key) {
    const auto found = j.find(key);
    return found != j.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

void pause(std::chrono::milliseconds wait, const CancellationToken& token) {
    for (const auto until = Clock::now() + wait; Clock::now() < until;) {
        if (token.cancelled()) throw std::runtime_error("cancelled");
        std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::milliseconds(50), until - Clock::now()));
    }
}

// MusicBrainz counts requests per client, so every lookup in the process shares one turn.
void waitForMusicBrainz(std::chrono::milliseconds interval, const CancellationToken& token) {
    static std::mutex mutex;
    static Clock::time_point next;
    std::lock_guard lock(mutex);
    pause(std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()), token);
    next = Clock::now() + interval;
}

// A 200 response's body; HTTP 429 and 503 are tried twice more after a pause, anything else throws.
std::string fetch(const std::string& url, bool musicbrainz, const Services& services, const CancellationToken& token,
                  const char* accept = "application/json", size_t maxBytes = kMaxJsonBytes) {
    for (int attempt = 0;; ++attempt) {
        if (musicbrainz) waitForMusicBrainz(services.musicbrainzInterval, token);
        auto response = http::get(url, accept, token, maxBytes);
        if (response.status == 200) return std::move(response.body);
        if ((response.status == 429 || response.status == 503) && attempt < 2) {
            pause(services.backoff * (attempt + 1), token);
            continue;
        }
        throw std::runtime_error("HTTP " + std::to_string(response.status));
    }
}

std::string describe(const json& artist) {
    if (auto description = text(artist, "disambiguation"); !description.empty()) return description;
    std::string out;
    const auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!out.empty()) out += " · ";
        out += part;
    };
    add(text(artist, "type"));
    if (const auto area = artist.find("area"); area != artist.end() && area->is_object()) add(text(*area, "name"));
    if (const auto life = artist.find("life-span"); life != artist.end() && life->is_object())
        add(text(*life, "begin").substr(0, 4));
    return out;
}

} // namespace

std::vector<ArtistCandidate> matchingArtists(const std::string& searchJson, const std::string& name) {
    const auto j = json::parse(searchJson);
    std::vector<ArtistCandidate> out;
    const auto artists = j.find("artists");
    if (artists == j.end() || !artists->is_array()) return out;
    for (const auto& artist : *artists) {
        if (!artist.is_object()) continue;
        bool named = text(artist, "name") == name;
        if (const auto aliases = artist.find("aliases"); !named && aliases != artist.end() && aliases->is_array())
            for (const auto& alias : *aliases) named = named || (alias.is_object() && text(alias, "name") == name);
        const auto mbid = text(artist, "id");
        if (named && !mbid.empty()) out.push_back({mbid, text(artist, "name"), describe(artist)});
    }
    return out;
}

std::optional<std::string> wikidataItem(const std::string& artistJson) {
    const auto j = json::parse(artistJson);
    const auto relations = j.find("relations");
    if (relations == j.end() || !relations->is_array()) return std::nullopt;
    static const std::regex item(R"(^https?://www\.wikidata\.org/wiki/(Q[0-9]+)$)");
    for (const auto& relation : *relations) {
        if (!relation.is_object() || text(relation, "type") != "wikidata") continue;
        const auto url = relation.find("url");
        if (url == relation.end() || !url->is_object()) continue;
        std::smatch match;
        const auto resource = text(*url, "resource");
        if (std::regex_match(resource, match, item)) return match[1].str();
    }
    return std::nullopt;
}

std::optional<std::string> pictureFile(const std::string& claimsJson) {
    const auto j = json::parse(claimsJson);
    const auto claims = j.find("claims");
    if (claims == j.end() || !claims->is_object()) return std::nullopt;
    const auto images = claims->find("P18");
    if (images == claims->end() || !images->is_array()) return std::nullopt;
    std::optional<std::string> normal;
    for (const auto& claim : *images) {
        if (!claim.is_object()) continue;
        const auto rank = text(claim, "rank");
        const auto snak = claim.find("mainsnak");
        if (rank == "deprecated" || snak == claim.end() || !snak->is_object()) continue;
        const auto value = snak->find("datavalue");
        if (value == snak->end() || !value->is_object()) continue;
        auto file = text(*value, "value");
        if (file.empty()) continue;
        if (rank == "preferred") return file;
        if (!normal) normal = file;
    }
    return normal;
}

std::optional<CommonsImage> commonsImage(const std::string& queryJson) {
    const auto j = json::parse(queryJson);
    const auto query = j.find("query");
    if (query == j.end() || !query->is_object()) return std::nullopt;
    const auto pages = query->find("pages");
    if (pages == query->end() || !pages->is_array()) return std::nullopt;
    for (const auto& page : *pages) {
        const auto infos = page.is_object() ? page.find("imageinfo") : page.end();
        if (!page.is_object() || infos == page.end() || !infos->is_array() || infos->empty() || !infos->front().is_object())
            continue;
        const auto& info = infos->front();
        CommonsImage image;
        image.url = text(info, "thumburl");
        if (image.url.empty()) image.url = text(info, "url");
        image.page = text(info, "descriptionurl");
        if (const auto meta = info.find("extmetadata"); meta != info.end() && meta->is_object()) {
            const auto field = [&](const char* name) {
                const auto entry = meta->find(name);
                return entry != meta->end() && entry->is_object() ? text(*entry, "value") : std::string{};
            };
            image.author = plainText(field("Artist"));
            image.license = plainText(field("LicenseShortName"));
            image.licenseUrl = field("LicenseUrl");
        }
        if (!image.url.empty()) return image;
    }
    return std::nullopt;
}

std::string plainText(std::string_view html) {
    std::string out;
    bool tag = false, space = false;
    for (size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (tag) {
            tag = c != '>';
            continue;
        }
        if (c == '<') {
            tag = true;
            continue;
        }
        std::string piece(1, c);
        if (c == '&') {
            const auto end = html.find(';', i);
            if (end != std::string_view::npos && end - i <= 10) {
                const auto entity = html.substr(i + 1, end - i - 1);
                uint32_t code = 0;
                if (entity == "amp")
                    code = '&';
                else if (entity == "lt")
                    code = '<';
                else if (entity == "gt")
                    code = '>';
                else if (entity == "quot")
                    code = '"';
                else if (entity == "apos" || entity == "#39")
                    code = '\'';
                else if (entity == "nbsp")
                    code = ' ';
                else if (entity.size() > 1 && entity[0] == '#')
                    code = uint32_t(std::strtoul(std::string(entity.substr(entity[1] == 'x' ? 2 : 1)).c_str(), nullptr,
                                                 entity[1] == 'x' ? 16 : 10));
                if (code && code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF)) {
                    std::wstring wide;
                    if (code >= 0x10000) {
                        wide += wchar_t(0xD800 + ((code - 0x10000) >> 10));
                        wide += wchar_t(0xDC00 + ((code - 0x10000) & 0x3FF));
                    } else {
                        wide += wchar_t(code);
                    }
                    piece = toUtf8(wide);
                    i = end;
                }
            }
        }
        if (piece == " " || piece == "\n" || piece == "\r" || piece == "\t") {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += piece;
    }
    return out;
}

Found pictureOf(const std::string& mbid, const Services& services, const CancellationToken& token) {
    Found found;
    found.mbid = mbid;
    const auto item = wikidataItem(
        fetch(services.musicbrainz + "/ws/2/artist/" + http::encode(mbid) + "?inc=url-rels&fmt=json", true, services, token));
    if (!item) return found;
    const auto file =
        pictureFile(fetch(services.wikidata + "/w/api.php?action=wbgetclaims&entity=" + *item + "&property=P18&format=json",
                          false, services, token));
    if (!file) return found;
    found.image = commonsImage(fetch(services.commons + "/w/api.php?action=query&titles=" + http::encode("File:" + *file) +
                                         "&prop=imageinfo&iiprop=url%7Cextmetadata&iiurlwidth=960"
                                         "&iiextmetadatafilter=Artist%7CLicenseShortName%7CLicenseUrl&format=json"
                                         "&formatversion=2",
                                     false, services, token));
    if (!found.image) return found;
    if (!found.image->url.starts_with("https://") && !(services.allowHttp && found.image->url.starts_with("http://"))) {
        found.image.reset();
        return found;
    }
    const auto bytes = fetch(found.image->url, false, services, token, "image/*", kMaxImageBytes);
    found.bytes.assign(bytes.begin(), bytes.end());
    return found;
}

Found findArtistPicture(const std::string& name, const Services& services, const CancellationToken& token) {
    std::string phrase;
    for (const char c : name) {
        if (c == '"' || c == '\\') phrase += '\\';
        phrase += c;
    }
    const auto query = "artist:\"" + phrase + "\" OR alias:\"" + phrase + "\"";
    auto candidates = matchingArtists(
        fetch(services.musicbrainz + "/ws/2/artist?query=" + http::encode(query) + "&limit=25&fmt=json", true, services, token),
        name);
    if (candidates.size() != 1) {
        Found found;
        found.candidates = std::move(candidates);
        return found;
    }
    auto found = pictureOf(candidates.front().mbid, services, token);
    found.candidates = std::move(candidates);
    return found;
}

} // namespace camel::server::online
