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

constexpr const char* kStateKey = "session";
constexpr const char* kOrderKey = "session_order";

void setState(sqlite::Database& db, const char* key, const std::string& value) {
    db.prepare("INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        .bind(1, key)
        .bind(2, value)
        .run();
}

std::optional<std::string> state(sqlite::Database& db, const char* key) {
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

void saveSession(sqlite::Database& db, const SavedSession& session, SessionParts parts) {
    if (parts.entries) {
        db.exec("DELETE FROM queue");
        auto insert = db.prepare("INSERT INTO queue(position,entry_id,file_id,path,start_frame,end_frame,decoder,source_description) "
                                 "VALUES(?,?,?,?,?,?,?,?)");
        for (size_t i = 0; i < session.entries.size(); ++i) {
            const auto& entry = session.entries[i];
            insert.bind(1, int64_t(i)).bind(2, int64_t(i) + 1);
            if (entry.file) insert.bind(3, entry.file);
            else insert.bindNull(3);
            insert.bind(4, entry.source.pathUtf8()).bind(5, int64_t(entry.source.range().first));
            if (const auto end = entry.source.range().endExclusive) insert.bind(6, int64_t(*end));
            else insert.bindNull(6);
            if (const auto decoder = entry.source.decoder()) insert.bind(7, int64_t(*decoder));
            else insert.bindNull(7);
            insert.bind(8, entry.description);
            insert.run();
            insert.reset();
        }
    }
    if (parts.order) setState(db, kOrderKey, nlohmann::json(session.order).dump());
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
    setState(db, kStateKey, j.dump());
}

bool hasSavedSession(sqlite::Database& db) {
    return state(db, kStateKey).has_value();
}

std::optional<SavedSession> loadSession(sqlite::Database& db) {
    const auto text = state(db, kStateKey);
    if (!text) return std::nullopt;
    try {
        SavedSession session;
        auto rows = db.prepare("SELECT path,start_frame,end_frame,decoder,coalesce(file_id,0),source_description FROM queue "
                               "ORDER BY position");
        while (rows.step()) {
            FrameRange range{uint64_t(rows.integer(1))};
            if (!rows.isNull(2)) range.endExclusive = uint64_t(rows.integer(2));
            std::optional<audio::DecoderKind> decoder;
            if (!rows.isNull(3) && uint64_t(rows.integer(3)) < std::size(audio::kAllDecoders))
                decoder = audio::DecoderKind(rows.integer(3));
            session.entries.push_back({TrackSource(rows.text(0), range, decoder), rows.integer(4), rows.text(5)});
        }
        const auto count = session.entries.size();
        const auto j = nlohmann::json::parse(*text);
        if (const auto order = state(db, kOrderKey)) session.order = indexes(nlohmann::json::parse(*order), count);
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
        if (j.contains("format") && j["format"].get<int>() <= int(audio::AudioFormat::Dsd))
            session.track.format = audio::AudioFormat(j["format"].get<int>());
        return session;
    } catch (const std::exception& e) {
        log::warn("session: saved queue cannot be read: %s", e.what());
        return std::nullopt;
    }
}

} // namespace camel::server
