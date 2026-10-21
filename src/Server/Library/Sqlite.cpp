// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/Sqlite.h"

#include <sqlite3.h>

#include <utility>

#include "Common/util/Text.h"

namespace camel::server::sqlite {
namespace {

[[noreturn]] void fail(sqlite3* db, int code, std::string_view what) {
    throw Error(code, std::string(what) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(code)));
}

void check(sqlite3* db, int code, std::string_view what) {
    if (code != SQLITE_OK) fail(db, code, what);
}

} // namespace

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
    check(db_, sqlite3_prepare_v2(db_, sql.data(), int(sql.size()), &stmt_, nullptr), "prepare");
}

Statement::~Statement() {
    sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::bind(int index, int64_t value) {
    check(db_, sqlite3_bind_int64(stmt_, index, value), "bind");
    return *this;
}

Statement& Statement::bind(int index, double value) {
    check(db_, sqlite3_bind_double(stmt_, index, value), "bind");
    return *this;
}

Statement& Statement::bind(int index, std::string_view text) {
    check(db_, sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind"); // NOLINT
    return *this;
}

Statement& Statement::bind(int index, std::span<const uint8_t> blob) {
    check(db_, sqlite3_bind_blob64(stmt_, index, blob.data(), blob.size(), SQLITE_TRANSIENT), "bind"); // NOLINT
    return *this;
}

Statement& Statement::bindNull(int index) {
    check(db_, sqlite3_bind_null(stmt_, index), "bind");
    return *this;
}

bool Statement::step() {
    const int code = sqlite3_step(stmt_);
    if (code == SQLITE_ROW) return true;
    if (code == SQLITE_DONE) return false;
    fail(db_, code, "step");
}

void Statement::run() {
    while (step()) {
    }
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Statement::isNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

int64_t Statement::integer(int column) const {
    return sqlite3_column_int64(stmt_, column);
}

double Statement::real(int column) const {
    return sqlite3_column_double(stmt_, column);
}

std::string Statement::text(int column) const {
    const auto* p = sqlite3_column_text(stmt_, column);
    return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(stmt_, column))) : std::string();
}

std::vector<uint8_t> Statement::blob(int column) const {
    const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, column));
    return p ? std::vector<uint8_t>(p, p + sqlite3_column_bytes(stmt_, column)) : std::vector<uint8_t>();
}

Database::Database(const std::filesystem::path& path, Mode mode) {
    const int flags = mode == Mode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    const int code = sqlite3_open_v2(toUtf8(path.wstring()).c_str(), &db_, flags | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(code);
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error(code, "open " + toUtf8(path.wstring()) + ": " + message);
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, 5000);
}

Database::~Database() {
    sqlite3_close(db_);
}

Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

void Database::exec(std::string_view sql) {
    const std::string text(sql);
    char* message = nullptr;
    const int code = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (code != SQLITE_OK) {
        const std::string detail = message ? message : sqlite3_errstr(code);
        sqlite3_free(message);
        throw Error(code, "exec: " + detail);
    }
}

Statement Database::prepare(std::string_view sql) {
    return Statement(db_, sql);
}

std::optional<int64_t> Database::scalar(std::string_view sql) {
    auto s = prepare(sql);
    if (!s.step() || s.isNull(0)) return std::nullopt;
    return s.integer(0);
}

int64_t Database::lastInsertId() const {
    return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const {
    return sqlite3_changes(db_);
}

int Database::userVersion() {
    return int(scalar("PRAGMA user_version").value_or(0));
}

void Database::setUserVersion(int version) {
    exec("PRAGMA user_version = " + std::to_string(version));
}

void Database::backupTo(const std::filesystem::path& target) {
    Database out(target);
    sqlite3_backup* backup = sqlite3_backup_init(out.db_, "main", db_, "main");
    if (!backup) fail(out.db_, sqlite3_errcode(out.db_), "backup");
    const int step = sqlite3_backup_step(backup, -1);
    const int finish = sqlite3_backup_finish(backup);
    if (step != SQLITE_DONE) fail(out.db_, step, "backup");
    check(out.db_, finish, "backup");
}

Transaction::Transaction(Database& db) : db_(db) {
    db_.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    // A failed statement may already have ended the transaction, so the result is not checked.
    if (!done_) sqlite3_exec(db_.handle(), "ROLLBACK", nullptr, nullptr, nullptr);
}

void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

} // namespace camel::server::sqlite
