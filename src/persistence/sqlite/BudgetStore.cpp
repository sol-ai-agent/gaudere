#include <gaudere/persistence/sqlite/BudgetStore.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace gaudere::persistence::sqlite {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3* db, sqlite3_stmt* statement, int index,
               const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT)
        != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

std::int64_t milliseconds(const budget::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::uint64_t count_rows(sqlite3* db, sqlite3_stmt* statement)
{
    const int result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
}

budget::ConsumeResult commit_result(sqlite3* db, budget::ConsumeResult result)
{
    execute(db, "COMMIT;");
    return result;
}

} // namespace

BudgetStore::BudgetStore(const std::string& path)
{
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_)
                                              : "cannot open SQLite";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }

    try {
        execute(database_, "PRAGMA journal_mode=WAL;");
        execute(database_, "PRAGMA synchronous=FULL;");
        execute(database_, "PRAGMA busy_timeout=5000;");
        execute(database_, "BEGIN IMMEDIATE;");
        execute(database_,
            "CREATE TABLE IF NOT EXISTS budget_consumptions ("
            " scope TEXT NOT NULL,"
            " idempotency_key TEXT NOT NULL,"
            " consumed_at_ms INTEGER NOT NULL,"
            " PRIMARY KEY(scope, idempotency_key)"
            ");");
        execute(database_,
            "CREATE INDEX IF NOT EXISTS idx_budget_consumptions_scope_time "
            "ON budget_consumptions(scope, consumed_at_ms);");
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

BudgetStore::~BudgetStore()
{
    sqlite3_close(database_);
}

budget::ConsumeResult BudgetStore::consume(
    const std::string& scope,
    const std::string& idempotency_key,
    const budget::TimePoint now,
    const budget::Policy& policy)
{
    if (scope.empty()) {
        throw std::invalid_argument("budget scope must not be empty");
    }
    if (idempotency_key.empty()) {
        throw std::invalid_argument("budget idempotency key must not be empty");
    }
    if (!budget::valid_policy(policy)) {
        throw std::invalid_argument("invalid budget policy");
    }

    execute(database_, "BEGIN IMMEDIATE;");
    try {
        {
            Statement statement(database_,
                "SELECT 1 FROM budget_consumptions "
                "WHERE scope=?1 AND idempotency_key=?2 LIMIT 1");
            bind_text(database_, statement.get(), 1, scope);
            bind_text(database_, statement.get(), 2, idempotency_key);
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_ROW) {
                return commit_result(database_, budget::ConsumeResult::duplicate);
            }
            if (result != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
        }

        {
            Statement statement(database_,
                "SELECT COUNT(*) FROM budget_consumptions WHERE scope=?1");
            bind_text(database_, statement.get(), 1, scope);
            if (count_rows(database_, statement.get()) >= policy.max_total) {
                return commit_result(database_, budget::ConsumeResult::total_exhausted);
            }
        }

        const std::int64_t now_ms = milliseconds(now);
        bool has_latest = false;
        std::int64_t latest_ms = 0;
        {
            Statement statement(database_,
                "SELECT MAX(consumed_at_ms) FROM budget_consumptions WHERE scope=?1");
            bind_text(database_, statement.get(), 1, scope);
            const int result = sqlite3_step(statement.get());
            if (result != SQLITE_ROW) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
            if (sqlite3_column_type(statement.get(), 0) != SQLITE_NULL) {
                has_latest = true;
                latest_ms = sqlite3_column_int64(statement.get(), 0);
            }
        }

        if (has_latest && latest_ms > now_ms) {
            return commit_result(database_, budget::ConsumeResult::clock_rollback);
        }
        if (has_latest
            && now_ms - latest_ms < policy.min_interval.count()) {
            return commit_result(database_, budget::ConsumeResult::cooldown);
        }

        {
            const auto cutoff = now - policy.window;
            Statement statement(database_,
                "SELECT COUNT(*) FROM budget_consumptions "
                "WHERE scope=?1 AND consumed_at_ms>?2");
            bind_text(database_, statement.get(), 1, scope);
            sqlite3_bind_int64(statement.get(), 2, milliseconds(cutoff));
            if (count_rows(database_, statement.get()) >= policy.max_in_window) {
                return commit_result(database_, budget::ConsumeResult::window_exhausted);
            }
        }

        {
            Statement statement(database_,
                "INSERT INTO budget_consumptions(scope,idempotency_key,consumed_at_ms) "
                "VALUES(?1,?2,?3)");
            bind_text(database_, statement.get(), 1, scope);
            bind_text(database_, statement.get(), 2, idempotency_key);
            sqlite3_bind_int64(statement.get(), 3, now_ms);
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
        }

        return commit_result(database_, budget::ConsumeResult::accepted);
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

} // namespace gaudere::persistence::sqlite
