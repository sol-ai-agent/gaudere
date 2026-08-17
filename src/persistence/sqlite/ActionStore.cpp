#include <gaudere/persistence/sqlite/ActionStore.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere::persistence::sqlite {
namespace {

using WakeAction = scheduling::wake::Action;
using ActionStatus = scheduling::wake::ActionStatus;
using EffectResult = scheduling::wake::EffectResult;
using Lease = scheduling::wake::Lease;
using TimePoint = scheduling::wake::TimePoint;

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }
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

std::int64_t milliseconds(const TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

TimePoint time_point(const std::int64_t value)
{
    return TimePoint{std::chrono::milliseconds{value}};
}

void bind_text(sqlite3* db, sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

WakeAction read_action(sqlite3_stmt* statement)
{
    WakeAction action;
    action.id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    action.idempotency_key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    action.critical = sqlite3_column_int(statement, 2) != 0;
    action.status = static_cast<ActionStatus>(sqlite3_column_int(statement, 3));
    action.effect_result = static_cast<EffectResult>(sqlite3_column_int(statement, 4));
    if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
        action.lease = Lease{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)),
            time_point(sqlite3_column_int64(statement, 6))};
    }
    return action;
}

constexpr const char* columns =
    "id, idempotency_key, critical, status, effect_result, "
    "lease_owner, lease_expires_at_ms";

} // namespace

ActionStore::ActionStore(const std::string& path)
{
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_) : "cannot open SQLite";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        execute(database_, "PRAGMA journal_mode=WAL;");
        execute(database_, "PRAGMA synchronous=FULL;");
        execute(database_, "PRAGMA busy_timeout=5000;");
        Statement version_statement(database_, "PRAGMA user_version");
        if (sqlite3_step(version_statement.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        const int version = sqlite3_column_int(version_statement.get(), 0);
        if (version > 2) {
            throw std::runtime_error("unsupported SQLite schema version");
        }
        execute(database_, "BEGIN IMMEDIATE;");
        execute(database_,
            "CREATE TABLE IF NOT EXISTS actions ("
            " id TEXT PRIMARY KEY NOT NULL,"
            " idempotency_key TEXT NOT NULL UNIQUE,"
            " critical INTEGER NOT NULL CHECK(critical IN (0,1)),"
            " status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 5),"
            " effect_result INTEGER NOT NULL CHECK(effect_result BETWEEN 0 AND 2),"
            " lease_owner TEXT,"
            " lease_expires_at_ms INTEGER,"
            " CHECK((lease_owner IS NULL) = (lease_expires_at_ms IS NULL))"
            ");");
        if (version == 0) {
            execute(database_, "PRAGMA user_version=1;");
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

ActionStore::~ActionStore()
{
    sqlite3_close(database_);
}

std::optional<WakeAction> ActionStore::find(const std::string& id) const
{
    const std::string sql = std::string{"SELECT "} + columns + " FROM actions WHERE id=?1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return read_action(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error(sqlite3_errmsg(database_));
}

std::optional<WakeAction> ActionStore::find_by_idempotency_key(const std::string& key) const
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM actions WHERE idempotency_key=?1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, key);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return read_action(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    throw std::runtime_error(sqlite3_errmsg(database_));
}

std::vector<WakeAction> ActionStore::running_with_expired_lease(const TimePoint now) const
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM actions WHERE status=?1 AND lease_expires_at_ms<=?2";
    Statement statement(database_, sql.c_str());
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(ActionStatus::running));
    sqlite3_bind_int64(statement.get(), 2, milliseconds(now));
    std::vector<WakeAction> actions;
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_ROW) actions.push_back(read_action(statement.get()));
        else if (result == SQLITE_DONE) return actions;
        else throw std::runtime_error(sqlite3_errmsg(database_));
    }
}

bool ActionStore::has_running() const
{
    Statement statement(database_, "SELECT 1 FROM actions WHERE status=?1 LIMIT 1");
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(ActionStatus::running));
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    throw std::runtime_error(sqlite3_errmsg(database_));
}

void ActionStore::save(const WakeAction& action)
{
    Statement statement(database_,
        "INSERT INTO actions (id,idempotency_key,critical,status,effect_result,"
        "lease_owner,lease_expires_at_ms) VALUES (?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(id) DO UPDATE SET "
        "idempotency_key=excluded.idempotency_key, critical=excluded.critical, "
        "status=excluded.status, effect_result=excluded.effect_result, "
        "lease_owner=excluded.lease_owner, lease_expires_at_ms=excluded.lease_expires_at_ms");
    bind_text(database_, statement.get(), 1, action.id);
    bind_text(database_, statement.get(), 2, action.idempotency_key);
    sqlite3_bind_int(statement.get(), 3, action.critical ? 1 : 0);
    sqlite3_bind_int(statement.get(), 4, static_cast<int>(action.status));
    sqlite3_bind_int(statement.get(), 5, static_cast<int>(action.effect_result));
    if (action.lease) {
        bind_text(database_, statement.get(), 6, action.lease->owner);
        sqlite3_bind_int64(statement.get(), 7, milliseconds(action.lease->expires_at));
    } else {
        sqlite3_bind_null(statement.get(), 6);
        sqlite3_bind_null(statement.get(), 7);
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
}

} // namespace gaudere::persistence::sqlite
