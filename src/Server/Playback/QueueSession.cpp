// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Playback/QueueSession.h"

#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>

#include "Common/util/Log.h"

namespace camel::server {
namespace {

// Each followed by ':' and the zone's ID.
constexpr const char* kStateKey = "session";
constexpr const char* kOrderKey = "session_order";
constexpr const char* kAutomaticKey = "session_automatic"; // positions of the entries autoplay added

std::string key(const char* name, const std::string& zone) {
    return std::string(name) + ":" + zone;
}

void setState(sqlite::Database& db, const std::string& key, const std::string& value) {
    db.prepare("INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        .bind(1, key)
        .bind(2, value)
        .run();
}

std::optional<std::string> state(sqlite::Database& db, const std::string& key) {
    auto row = db.prepare("SELECT value FROM app_state WHERE key=?");
    if (!row.bind(1, key).step()) return std::nullopt;
    return row.text(0);
}

std::vector<size_t> indexes(const nlohmann::json& list, size_t count) {
    std::vector<size_t> result;
    for (const auto& item : list)
        if (const auto at = item.get<size_t>(); at < count) result.push_back(at);
    return result;
}

} // namespace

size_t rowHash(const SavedQueueEntry& entry) {
    const auto& range = entry.source.range();
    const auto decoder = entry.source.decoder();
    size_t hash = std::hash<std::string>{}(entry.source.pathUtf8());
    for (const size_t part :
         {std::hash<std::string>{}(entry.description), size_t(range.first),
          range.endExclusive ? size_t(*range.endExclusive) + 1 : 0, decoder ? size_t(*decoder) + 1 : 0, size_t(entry.file)})
        hash ^= part + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

void saveSession(sqlite::Database& db, const std::string& zone, const SavedSession& session, SessionParts parts) {
    if (parts.entries) {
        // A scan deletes the rows of files that are gone, which leaves the entries they kept to be written again.
        auto kept = int64_t(std::min(parts.keptEntries, session.entries.size()));
        if (kept) {
            auto stored = db.prepare("SELECT count(*) FROM queue WHERE zone=? AND position<?");
            if (!stored.bind(1, zone).bind(2, kept).step() || stored.integer(0) != kept) kept = 0;
        }
        db.prepare("DELETE FROM queue WHERE zone=? AND position>=?").bind(1, zone).bind(2, kept).run();
        auto insert =
            db.prepare("INSERT INTO queue(zone,position,entry_id,file_id,path,start_frame,end_frame,decoder,source_description) "
                       "VALUES(?,?,?,(SELECT id FROM files WHERE id=?),?,?,?,?,?)"); // a file deleted meanwhile is unlinked
        for (auto i = size_t(kept); i < session.entries.size(); ++i) {
            const auto& entry = session.entries[i];
            insert.bind(1, zone).bind(2, int64_t(i)).bind(3, int64_t(i) + 1);
            if (entry.file)
                insert.bind(4, entry.file);
            else
                insert.bindNull(4);
            insert.bind(5, entry.source.pathUtf8()).bind(6, int64_t(entry.source.range().first));
            if (const auto end = entry.source.range().endExclusive)
                insert.bind(7, int64_t(*end));
            else
                insert.bindNull(7);
            if (const auto decoder = entry.source.decoder())
                insert.bind(8, int64_t(*decoder));
            else
                insert.bindNull(8);
            insert.bind(9, entry.description);
            insert.run();
            insert.reset();
        }
        std::vector<size_t> automatic;
        for (size_t i = 0; i < session.entries.size(); ++i)
            if (session.entries[i].automatic) automatic.push_back(i);
        setState(db, key(kAutomaticKey, zone), nlohmann::json(automatic).dump());
    }
    if (parts.order) setState(db, key(kOrderKey, zone), nlohmann::json(session.order).dump());
    nlohmann::json j;
    j["history"] = session.history;
    if (session.current) j["current"] = *session.current;
    j["shuffle"] = session.shuffle;
    j["repeat"] = int(session.repeat);
    j["position"] = session.track.position;
    j["total"] = session.track.total;
    if (const auto& source = session.track.source)
        j["source"] = {source->sampleRate, source->channels, source->bitsPerSample, source->totalFrames};
    if (session.track.format) j["format"] = int(*session.track.format);
    setState(db, key(kStateKey, zone), j.dump());
}

bool hasSavedSession(sqlite::Database& db) {
    return db.prepare("SELECT 1 FROM app_state WHERE substr(key,1,8)='session:'").step();
}

bool hasSavedSession(sqlite::Database& db, const std::string& zone) {
    return state(db, key(kStateKey, zone)).has_value();
}

void deleteSession(sqlite::Database& db, const std::string& zone) {
    db.prepare("DELETE FROM queue WHERE zone=?").bind(1, zone).run();
    for (const char* name : {kStateKey, kOrderKey, kAutomaticKey})
        db.prepare("DELETE FROM app_state WHERE key=?").bind(1, key(name, zone)).run();
}

void moveSession(sqlite::Database& db, const std::string& from, const std::string& to) {
    if (from == to) return;
    deleteSession(db, to);
    db.prepare("UPDATE queue SET zone=? WHERE zone=?").bind(1, to).bind(2, from).run();
    for (const char* name : {kStateKey, kOrderKey, kAutomaticKey})
        db.prepare("UPDATE app_state SET key=? WHERE key=?").bind(1, key(name, to)).bind(2, key(name, from)).run();
}

std::optional<SavedSession> loadSession(sqlite::Database& db, const std::string& zone) {
    const auto text = state(db, key(kStateKey, zone));
    if (!text) return std::nullopt;
    try {
        SavedSession session;
        auto rows = db.prepare("SELECT path,start_frame,end_frame,decoder,coalesce(file_id,0),source_description FROM queue "
                               "WHERE zone=? ORDER BY position");
        rows.bind(1, zone);
        while (rows.step()) {
            FrameRange range{uint64_t(rows.integer(1))};
            if (!rows.isNull(2)) range.endExclusive = uint64_t(rows.integer(2));
            // A decoder since removed, such as dr_mp3, leaves the entry to the setting for its format.
            std::optional<audio::DecoderKind> decoder;
            if (!rows.isNull(3))
                for (const auto kind : audio::kAllDecoders)
                    if (int64_t(kind) == rows.integer(3)) decoder = kind;
            session.entries.push_back({TrackSource(rows.text(0), range, decoder), rows.integer(4), rows.text(5)});
        }
        const auto count = session.entries.size();
        const auto j = nlohmann::json::parse(*text);
        if (const auto order = state(db, key(kOrderKey, zone))) session.order = indexes(nlohmann::json::parse(*order), count);
        if (const auto automatic = state(db, key(kAutomaticKey, zone)))
            for (const auto at : indexes(nlohmann::json::parse(*automatic), count)) session.entries[at].automatic = true;
        session.history = indexes(j.value("history", nlohmann::json::array()), count);
        if (j.contains("current") && j["current"].get<size_t>() < count) session.current = j["current"].get<size_t>();
        session.shuffle = j.value("shuffle", false);
        session.repeat = RepeatMode(std::clamp(j.value("repeat", 0), 0, int(RepeatMode::All)));
        session.track.position = j.value("position", uint64_t(0));
        session.track.total = j.value("total", uint64_t(0));
        if (j.contains("source")) {
            const auto& s = j["source"];
            session.track.source = audio::SourceInfo{s.at(0).get<uint32_t>(), s.at(1).get<uint32_t>(), s.at(2).get<uint32_t>(),
                                                     s.at(3).get<uint64_t>()};
        }
        if (j.contains("format"))
            for (const auto format : audio::kAllFormats)
                if (int(format) == j["format"].get<int>()) session.track.format = format;
        return session;
    } catch (const std::exception& e) {
        log::warn("session: saved queue cannot be read: %s", e.what());
        return std::nullopt;
    }
}

} // namespace camel::server
