// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Library/LibraryNames.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include "Common/util/Text.h"
#include "Server/Library/LibraryText.h"
#include "Server/Library/Sqlite.h"

namespace camel::server {
namespace {

struct Token {
    std::wstring text;
    bool name = false;
};

struct Span {
    size_t begin = 0, end = 0;
};

bool space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == 0x3000;
}

bool opening(wchar_t c) {
    return c == L'(' || c == 0xFF08 || c == L'[' || c == 0xFF3B || c == 0x3010;
}

bool closing(wchar_t c) {
    return c == L')' || c == 0xFF09 || c == L']' || c == 0xFF3D || c == 0x3011;
}

std::wstring_view trimmed(std::wstring_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

std::string keyOf(std::wstring_view name) {
    const auto text = trimmed(name);
    if (text.empty()) return {};
    int count = NormalizeString(NormalizationKC, text.data(), int(text.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("cannot normalize a library name");
    std::wstring folded(size_t(count), L'\0');
    count = NormalizeString(NormalizationKC, text.data(), int(text.size()), folded.data(), count);
    if (count <= 0) throw std::runtime_error("cannot normalize a library name");
    folded.resize(size_t(count));
    return toUtf8(folded);
}

// Whether `word`, in lowercase ASCII, starts at s[i] in any case.
bool wordAt(std::wstring_view s, size_t i, std::wstring_view word) {
    if (i > s.size() || s.size() - i < word.size()) return false;
    for (size_t k = 0; k < word.size(); ++k)
        if (std::towlower(s[i + k]) != word[k]) return false;
    return true;
}

// The length of feat., ft., featuring or with at s[i] with the spaces after it, or 0. Outside a bracket the word must
// follow a space.
size_t keywordAt(std::wstring_view s, size_t i, bool openingBracket) {
    if (!openingBracket && (i == 0 || !space(s[i - 1]))) return 0;
    const auto spaced = [&](size_t end) { return end < s.size() && space(s[end]); };
    const auto through = [&](size_t end) {
        while (end < s.size() && space(s[end])) ++end;
        return end - i;
    };
    if (wordAt(s, i, L"featuring") && spaced(i + 9)) return through(i + 9);
    if (wordAt(s, i, L"feat.")) return through(i + 5);
    if (wordAt(s, i, L"feat") && spaced(i + 4)) return through(i + 4);
    if (wordAt(s, i, L"ft.")) return through(i + 3);
    if (wordAt(s, i, L"with") && spaced(i + 4)) return through(i + 4);
    return 0;
}

class Tokens {
public:
    explicit Tokens(std::vector<Token>& out) : out_(out) {}
    void separator(std::wstring_view text) {
        if (text.empty()) return;
        if (!out_.empty() && !out_.back().name)
            out_.back().text += text;
        else
            out_.push_back({std::wstring(text), false});
    }
    // A name keeps no spaces at its ends; they join the separators around it so the parts still give back the credit.
    void name(std::wstring_view text) {
        size_t lead = 0;
        while (lead < text.size() && space(text[lead])) ++lead;
        size_t trail = 0;
        while (trail < text.size() - lead && space(text[text.size() - 1 - trail])) ++trail;
        separator(text.substr(0, lead));
        if (lead + trail < text.size()) out_.push_back({std::wstring(text.substr(lead, text.size() - lead - trail)), true});
        separator(text.substr(text.size() - trail));
    }

private:
    std::vector<Token>& out_;
};

// Splits at the separators that always divide names. Text in brackets stays with its name unless the bracket opens with
// feat., ft., featuring or with, which then divides the names before and inside it.
void divide(std::wstring_view s, std::vector<Token>& out) {
    Tokens tokens(out);
    size_t start = 0, depth = 0;
    for (size_t i = 0; i < s.size();) {
        const wchar_t c = s[i];
        if (depth == 0 && opening(c)) {
            size_t word = i + 1;
            while (word < s.size() && space(s[word])) ++word;
            if (const size_t length = keywordAt(s, word, true)) {
                size_t close = word + length;
                for (size_t inner = 1; close < s.size(); ++close) {
                    if (opening(s[close])) ++inner;
                    if (closing(s[close]) && --inner == 0) break;
                }
                tokens.name(s.substr(start, i - start));
                tokens.separator(s.substr(i, word + length - i));
                divide(s.substr(word + length, close - word - length), out);
                if (close < s.size()) tokens.separator(s.substr(close, 1));
                i = start = std::min(close + 1, s.size());
                continue;
            }
        }
        if (opening(c)) {
            ++depth;
        } else if (closing(c)) {
            if (depth > 0) --depth;
        } else if (depth == 0) {
            size_t length = c == L';' || c == 0xFF1B || c == 0x3001 ? 1 : keywordAt(s, i, false);
            if (length) {
                tokens.name(s.substr(start, i - start));
                tokens.separator(s.substr(i, length));
                i = start = i + length;
                continue;
            }
        }
        ++i;
    }
    tokens.name(s.substr(start));
}

// & ＆ , ， / ／ × : ： and the words x, and and vs between spaces, outside brackets, each with the spaces around it.
std::vector<Span> ambiguousSeparators(std::wstring_view s) {
    std::vector<Span> spans;
    size_t depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (opening(c)) {
            ++depth;
            continue;
        }
        if (closing(c)) {
            if (depth > 0) --depth;
            continue;
        }
        if (depth > 0) continue;
        size_t length = 0;
        if (c == L'&' || c == 0xFF06 || c == L',' || c == 0xFF0C || c == L'/' || c == 0xFF0F || c == 0x00D7 || c == L':' ||
            c == 0xFF1A) {
            length = 1;
        } else if (i > 0 && space(s[i - 1])) {
            const auto spaced = [&](size_t end) { return end < s.size() && space(s[end]); };
            if ((c == L'x' || c == L'X') && spaced(i + 1))
                length = 1;
            else if ((wordAt(s, i, L"and") || wordAt(s, i, L"vs.")) && spaced(i + 3))
                length = 3;
            else if (wordAt(s, i, L"vs") && spaced(i + 2))
                length = 2;
        }
        if (!length) continue;
        size_t begin = i, end = i + length;
        const size_t floor = spans.empty() ? 0 : spans.back().end;
        while (begin > floor && space(s[begin - 1])) --begin;
        while (end < s.size() && space(s[end])) ++end;
        spans.push_back({begin, end});
        i = end - 1;
    }
    return spans;
}

} // namespace

std::string libraryNameKey(std::string_view name) {
    return keyOf(toWide(std::string(name)));
}

LibraryNameSplitter::LibraryNameSplitter(const std::vector<std::string>& credits) {
    for (const auto& credit : credits) learn(credit);
}

void LibraryNameSplitter::learn(std::string_view credit) {
    std::vector<Token> tokens;
    divide(toWide(std::string(credit)), tokens);
    for (const auto& token : tokens)
        if (token.name) known_.insert(keyOf(token.text));
}

std::vector<LibraryNamePart> LibraryNameSplitter::parts(std::string_view credit) const {
    std::vector<Token> divided, out;
    divide(toWide(std::string(credit)), divided);
    Tokens tokens(out);
    for (const auto& token : divided) {
        if (!token.name) {
            tokens.separator(token.text);
            continue;
        }
        // The finest division into consecutive groups of segments that are all known names; none leaves the name whole.
        const std::wstring_view s = token.text;
        const auto spans = ambiguousSeparators(s);
        const size_t segments = spans.size() + 1;
        const auto begin = [&](size_t k) { return k == 0 ? 0 : spans[k - 1].end; };
        const auto end = [&](size_t k) { return k == spans.size() ? s.size() : spans[k].begin; };
        std::vector<int> best(segments + 1, -1);
        std::vector<size_t> from(segments + 1, 0);
        best[0] = 0;
        for (size_t i = 1; i <= segments && !spans.empty(); ++i)
            for (size_t j = 0; j < i; ++j) {
                if (best[j] < 0 || best[j] + 1 <= best[i]) continue;
                if (!known_.contains(keyOf(s.substr(begin(j), end(i - 1) - begin(j))))) continue;
                best[i] = best[j] + 1;
                from[i] = j;
            }
        if (best[segments] < 2) {
            out.push_back(token);
            continue;
        }
        std::vector<size_t> starts;
        for (size_t i = segments; i > 0; i = from[i]) starts.insert(starts.begin(), from[i]);
        for (size_t g = 0; g < starts.size(); ++g) {
            const size_t last = (g + 1 < starts.size() ? starts[g + 1] : segments) - 1;
            if (g > 0) {
                const auto& span = spans[starts[g] - 1];
                tokens.separator(s.substr(span.begin, span.end - span.begin));
            }
            out.push_back({std::wstring(s.substr(begin(starts[g]), end(last) - begin(starts[g]))), true});
        }
    }
    std::vector<LibraryNamePart> parts;
    parts.reserve(out.size());
    for (const auto& token : out) parts.push_back({toUtf8(token.text), token.name});
    return parts;
}

std::vector<std::string> LibraryNameSplitter::names(std::string_view credit) const {
    std::vector<std::string> names;
    for (auto& part : parts(credit))
        if (part.name) names.push_back(std::move(part.text));
    return names;
}

LibraryNameSplitter loadLibraryNames(sqlite::Database& db) {
    std::vector<std::string> credits;
    auto rows = db.prepare("SELECT value FROM track_tags UNION SELECT album_artist FROM albums WHERE album_artist IS NOT NULL");
    while (rows.step()) credits.push_back(rows.text(0));
    return LibraryNameSplitter(credits);
}

size_t deriveLibraryNames(sqlite::Database& db, const LibraryNameSplitter& splitter, const std::vector<int64_t>& tracks) {
    struct Name {
        std::string value, sort;
        bool operator==(const Name&) const = default;
    };
    using Key = std::pair<int64_t, std::string>; // track and field
    std::string scope;
    for (const auto track : tracks) scope += (scope.empty() ? " AND track_id IN (" : ",") + std::to_string(track);
    if (!scope.empty()) scope += ")";

    // A tag's sort tag still applies when the tag is one name; the names split from it sort by themselves.
    std::map<Key, std::vector<Name>> wanted, stored;
    std::unordered_map<std::string, std::vector<std::string>> split;
    auto tags = db.prepare("SELECT track_id,field,value,coalesce(sort_source,'') FROM track_tags WHERE 1=1" + scope +
                           " ORDER BY track_id,field,position");
    while (tags.step()) {
        auto& names = wanted[{tags.integer(0), tags.text(1)}];
        const auto value = tags.text(2);
        auto [found, added] = split.try_emplace(value);
        if (added) found->second = splitter.names(value);
        const bool whole = found->second.size() == 1 && found->second.front() == value;
        for (const auto& name : found->second)
            if (std::none_of(names.begin(), names.end(), [&](const Name& n) { return n.value == name; }))
                names.push_back({name, whole ? tags.text(3) : std::string{}});
    }
    auto values = db.prepare("SELECT track_id,field,value,coalesce(sort_source,'') FROM track_values WHERE field IN "
                             "('artist','composer')" +
                             scope + " ORDER BY track_id,field,position");
    while (values.step()) stored[{values.integer(0), values.text(1)}].push_back({values.text(2), values.text(3)});

    auto state = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
    const std::wstring locale = state.step() ? toWide(state.text(0)) : L"zh-TW";
    auto remove = db.prepare("DELETE FROM track_values WHERE track_id=? AND field=?");
    auto insert = db.prepare("INSERT INTO track_values(track_id,field,position,value,sort_source,value_norm,sort_key) "
                             "VALUES(?,?,?,?,?,?,?)");
    size_t changed = 0;
    const auto rewrite = [&](const Key& key, const std::vector<Name>& names) {
        remove.bind(1, key.first).bind(2, key.second).run();
        remove.reset();
        int64_t position = 0;
        for (const auto& name : names) {
            insert.bind(1, key.first)
                .bind(2, key.second)
                .bind(3, position++)
                .bind(4, name.value)
                .bind(5, name.sort)
                .bind(6, normalizeLibrarySearch(name.value))
                .bind(7, librarySortKey(librarySortSource(name.sort, name.value), locale))
                .run();
            insert.reset();
        }
        ++changed;
    };
    for (const auto& [key, names] : wanted)
        if (const auto found = stored.find(key); found == stored.end() || found->second != names) rewrite(key, names);
    for (const auto& [key, names] : stored)
        if (!wanted.contains(key)) rewrite(key, {});
    if (changed) db.exec("UPDATE library_revision SET value=value+1 WHERE id=1");
    return changed;
}

void updateLibraryNameRules(sqlite::Database& db) {
    auto state = db.prepare("SELECT value FROM app_state WHERE key='name_rules'");
    if (state.step() && state.text(0) == kLibraryNameRules) return;
    deriveLibraryNames(db, loadLibraryNames(db));
    db.prepare("INSERT OR REPLACE INTO app_state(key,value) VALUES('name_rules',?)").bind(1, kLibraryNameRules).run();
}

} // namespace camel::server
