// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A thin RAII layer over the SQLite C API (docs/Plan.md §7.6).
#pragma once
#include <cstdint>
#include <filesystem>
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

class Database {
public:
    enum class Mode : uint8_t { ReadWrite, ReadOnly };
    Database(const std::filesystem::path& path, Mode mode = Mode::ReadWrite);
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

private:
    sqlite3* db_ = nullptr;
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
