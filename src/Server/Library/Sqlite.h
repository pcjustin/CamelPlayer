// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A thin RAII layer over the SQLite C API (docs/Plan.md §7.6).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace camel::server::sqlite {

class Error : public std::runtime_error {
public:
    Error(int code, const std::string& what) : std::runtime_error(what), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& operator=(Statement&&) = delete;

    // Parameters are 1-based, as in SQL.
    Statement& bind(int index, int64_t value);
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view text);
    Statement& bind(int index, std::span<const uint8_t> blob);
    Statement& bindNull(int index);

    bool step(); // true while a row is available
    void run();  // steps to completion
    void reset();

    // Columns are 0-based.
    bool isNull(int column) const;
    int64_t integer(int column) const;
    double real(int column) const;
    std::string text(int column) const;
    std::vector<uint8_t> blob(int column) const;

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class ConnectionPool;

class Database {
public:
    enum class Mode : uint8_t { ReadWrite, ReadOnly };
    Database(const std::filesystem::path& path, Mode mode = Mode::ReadWrite);
    // A connection `pool` gave, or that goes to it when destroyed rather than closing.
    Database(sqlite3* db, std::shared_ptr<ConnectionPool> pool);
    ~Database();
    Database(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database& operator=(Database&&) = delete;

    void exec(std::string_view sql); // one or more statements, no results
    Statement prepare(std::string_view sql);
    std::optional<int64_t> scalar(std::string_view sql); // first column of the first row
    int64_t lastInsertId() const;
    int changes() const;
    int userVersion();
    void setUserVersion(int version);
    // A consistent copy to `target` through the backup API; works while other connections read.
    void backupTo(const std::filesystem::path& target);
    sqlite3* handle() const { return db_; }
    // From now on the connection goes to `pool` when destroyed.
    void keepIn(std::shared_ptr<ConnectionPool> pool) { pool_ = std::move(pool); }

private:
    sqlite3* db_ = nullptr;
    std::shared_ptr<ConnectionPool> pool_;
};

// Open connections kept for the next user, who finds them with the schema parsed and the pages they read. A connection
// comes back with no transaction and no progress handler; once the pool is closed, those that come back are closed.
class ConnectionPool {
public:
    explicit ConnectionPool(size_t kept) : kept_(kept) {}
    ~ConnectionPool() { close(); }
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // A kept connection, or none.
    sqlite3* take();
    void give(sqlite3* db);
    void close();

private:
    const size_t kept_;
    std::mutex mutex_;
    std::vector<sqlite3*> idle_;
    bool closed_ = false;
};

// BEGIN IMMEDIATE on construction; rolls back unless commit() was called.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

} // namespace camel::server::sqlite
