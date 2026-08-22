#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace gaudere::persistence::sqlite {
namespace {

using scheduling::wake::WakeIntent;
using scheduling::wake::WakeIntentAcceptResult;
using scheduling::wake::WakeIntentPolicy;
using scheduling::wake::WakeIntentReconcileResult;
using scheduling::wake::WakeIntentRevokeResult;
using scheduling::wake::WakeIntentStatus;
using scheduling::wake::WakeIntentTimePoint;

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3* database,
               sqlite3_stmt* statement,
               const int index,
               const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

std::int64_t milliseconds(const WakeIntentTimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

WakeIntentTimePoint time_point(const std::int64_t value)
{
    return WakeIntentTimePoint{std::chrono::milliseconds{value}};
}

WakeIntent read_intent(sqlite3_stmt* statement)
{
    WakeIntent intent;
    intent.scope = text(statement, 0);
    intent.id = text(statement, 1);
    intent.source_id = text(statement, 2);
    intent.accepted_at = time_point(sqlite3_column_int64(statement, 3));
    intent.due_at = time_point(sqlite3_column_int64(statement, 4));
    const int status = sqlite3_column_int(statement, 5);
    if (status < static_cast<int>(WakeIntentStatus::scheduled)
        || status > static_cast<int>(WakeIntentStatus::manual_review)) {
        throw std::runtime_error("invalid persisted wake-intent status");
    }
    intent.status = static_cast<WakeIntentStatus>(status);
    if (sqlite3_column_type(statement, 6) != SQLITE_NULL) {
        intent.terminal_at = time_point(sqlite3_column_int64(statement, 6));
    }
    intent.terminal_reason = text(statement, 7);
    if (!scheduling::wake::valid_wake_intent(intent)) {
        throw std::runtime_error("invalid persisted wake-intent record");
    }
    return intent;
}

constexpr const char* columns =
    "scope,id,source_id,accepted_at_ms,due_at_ms,status,"
    "terminal_at_ms,terminal_reason";

template <typename Result>
Result commit_result(sqlite3* database, const Result result)
{
    execute(database, "COMMIT;");
    return result;
}

std::optional<WakeIntent> find_one(sqlite3* database,
                                   const char* where,
                                   const std::string& scope,
                                   const std::string& value)
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM wake_intents WHERE scope=?1 AND " + where + "=?2";
    Statement statement(database, sql.c_str());
    bind_text(database, statement.get(), 1, scope);
    bind_text(database, statement.get(), 2, value);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        return read_intent(statement.get());
    }
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    throw std::runtime_error(sqlite3_errmsg(database));
}

void transition(sqlite3* database,
                const std::string& scope,
                const std::string& id,
                const WakeIntentStatus status,
                const WakeIntentTimePoint terminal_at,
                const std::string& reason)
{
    Statement statement(database,
        "UPDATE wake_intents SET status=?3,terminal_at_ms=?4,terminal_reason=?5 "
        "WHERE scope=?1 AND id=?2 AND status=?6");
    bind_text(database, statement.get(), 1, scope);
    bind_text(database, statement.get(), 2, id);
    sqlite3_bind_int(statement.get(), 3, static_cast<int>(status));
    sqlite3_bind_int64(statement.get(), 4, milliseconds(terminal_at));
    bind_text(database, statement.get(), 5, reason);
    sqlite3_bind_int(statement.get(), 6,
                     static_cast<int>(WakeIntentStatus::scheduled));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    if (sqlite3_changes(database) != 1) {
        throw std::runtime_error("wake-intent terminal transition conflict");
    }
}

bool valid_observation(const std::string& scope,
                       const WakeIntentTimePoint now) noexcept
{
    return scheduling::wake::valid_wake_intent_identifier(scope)
        && std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch()) == now.time_since_epoch();
}

} // namespace

WakeIntentStore::WakeIntentStore(const std::string& path)
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

        Statement version_statement(database_, "PRAGMA user_version");
        if (sqlite3_step(version_statement.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        const int version = sqlite3_column_int(version_statement.get(), 0);
        if (version > 4) {
            throw std::runtime_error("unsupported SQLite schema version");
        }

        execute(database_,
            "CREATE TABLE IF NOT EXISTS wake_intents ("
            " scope TEXT NOT NULL,"
            " id TEXT NOT NULL,"
            " source_id TEXT NOT NULL,"
            " accepted_at_ms INTEGER NOT NULL,"
            " due_at_ms INTEGER NOT NULL,"
            " status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 3),"
            " terminal_at_ms INTEGER,"
            " terminal_reason TEXT NOT NULL,"
            " PRIMARY KEY(scope,id),"
            " UNIQUE(scope,source_id),"
            " CHECK(length(CAST(scope AS BLOB)) BETWEEN 1 AND 128),"
            " CHECK(length(CAST(id AS BLOB)) BETWEEN 1 AND 128),"
            " CHECK(length(CAST(source_id AS BLOB)) BETWEEN 1 AND 128),"
            " CHECK(due_at_ms > accepted_at_ms),"
            " CHECK((status=0) = (terminal_at_ms IS NULL)),"
            " CHECK(status!=1 OR terminal_at_ms>=due_at_ms),"
            " CHECK(status!=2 OR (terminal_at_ms>=accepted_at_ms "
            "                     AND terminal_at_ms<due_at_ms)),"
            " CHECK((status IN (0,1)) = "
            "       (length(CAST(terminal_reason AS BLOB))=0)),"
            " CHECK(length(CAST(terminal_reason AS BLOB)) <= 1024)"
            ");");
        execute(database_,
            "CREATE INDEX IF NOT EXISTS idx_wake_intents_scope_status_due "
            "ON wake_intents(scope,status,due_at_ms);");
        execute(database_,
            "CREATE TRIGGER IF NOT EXISTS wake_intents_require_scheduled_insert "
            "BEFORE INSERT ON wake_intents "
            "WHEN NEW.status!=0 OR NEW.terminal_at_ms IS NOT NULL "
            " OR length(CAST(NEW.terminal_reason AS BLOB))!=0 "
            "BEGIN "
            " SELECT RAISE(ABORT,'wake intent must be inserted scheduled'); "
            "END;");
        execute(database_,
            "CREATE TRIGGER IF NOT EXISTS wake_intents_single_transition "
            "BEFORE UPDATE ON wake_intents "
            "WHEN OLD.status!=0 OR NEW.status=0 "
            " OR NEW.scope IS NOT OLD.scope OR NEW.id IS NOT OLD.id "
            " OR NEW.source_id IS NOT OLD.source_id "
            " OR NEW.accepted_at_ms IS NOT OLD.accepted_at_ms "
            " OR NEW.due_at_ms IS NOT OLD.due_at_ms "
            "BEGIN "
            " SELECT RAISE(ABORT,'invalid wake intent transition'); "
            "END;");
        execute(database_,
            "CREATE TRIGGER IF NOT EXISTS wake_intents_prevent_delete "
            "BEFORE DELETE ON wake_intents "
            "BEGIN "
            " SELECT RAISE(ABORT,'wake intents cannot be deleted'); "
            "END;");
        if (version < 4) {
            execute(database_, "PRAGMA user_version=4;");
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

WakeIntentStore::~WakeIntentStore()
{
    sqlite3_close(database_);
}

std::optional<WakeIntent> WakeIntentStore::find(
    const std::string& scope,
    const std::string& id) const
{
    return find_one(database_, "id", scope, id);
}

std::optional<WakeIntent> WakeIntentStore::find_by_source(
    const std::string& scope,
    const std::string& source_id) const
{
    return find_one(database_, "source_id", scope, source_id);
}

WakeIntentAcceptResult WakeIntentStore::accept(
    const WakeIntent& intent,
    const WakeIntentPolicy& policy)
{
    if (!scheduling::wake::valid_new_wake_intent(intent)
        || !scheduling::wake::valid_wake_intent_policy(policy)) {
        return WakeIntentAcceptResult::invalid;
    }

    execute(database_, "BEGIN IMMEDIATE;");
    try {
        if (find_by_source(intent.scope, intent.source_id)) {
            return commit_result(database_, WakeIntentAcceptResult::duplicate);
        }
        if (find(intent.scope, intent.id)) {
            return commit_result(database_, WakeIntentAcceptResult::conflict);
        }

        Statement count(database_,
            "SELECT COUNT(*) FROM wake_intents WHERE scope=?1");
        bind_text(database_, count.get(), 1, intent.scope);
        if (sqlite3_step(count.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        const auto used = static_cast<std::uint64_t>(
            sqlite3_column_int64(count.get(), 0));
        if (used >= policy.max_total) {
            return commit_result(database_, WakeIntentAcceptResult::total_exhausted);
        }

        Statement insert(database_,
            "INSERT INTO wake_intents (scope,id,source_id,accepted_at_ms,due_at_ms,"
            "status,terminal_at_ms,terminal_reason) "
            "VALUES (?1,?2,?3,?4,?5,?6,NULL,'')");
        bind_text(database_, insert.get(), 1, intent.scope);
        bind_text(database_, insert.get(), 2, intent.id);
        bind_text(database_, insert.get(), 3, intent.source_id);
        sqlite3_bind_int64(insert.get(), 4, milliseconds(intent.accepted_at));
        sqlite3_bind_int64(insert.get(), 5, milliseconds(intent.due_at));
        sqlite3_bind_int(insert.get(), 6,
                         static_cast<int>(WakeIntentStatus::scheduled));
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        return commit_result(database_, WakeIntentAcceptResult::accepted);
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

std::optional<WakeIntentTimePoint> WakeIntentStore::next_scheduled_at(
    const std::string& scope) const
{
    if (!scheduling::wake::valid_wake_intent_identifier(scope)) {
        throw std::invalid_argument("invalid wake-intent scope");
    }
    Statement statement(database_,
        "SELECT MIN(due_at_ms) FROM wake_intents WHERE scope=?1 AND status=?2");
    bind_text(database_, statement.get(), 1, scope);
    sqlite3_bind_int(statement.get(), 2,
                     static_cast<int>(WakeIntentStatus::scheduled));
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL) {
        return std::nullopt;
    }
    return time_point(sqlite3_column_int64(statement.get(), 0));
}

WakeIntentReconcileResult WakeIntentStore::reconcile(
    const std::string& scope,
    const WakeIntentTimePoint now)
{
    if (!valid_observation(scope, now)) {
        throw std::invalid_argument("invalid wake-intent reconciliation input");
    }

    execute(database_, "BEGIN IMMEDIATE;");
    try {
        WakeIntentReconcileResult result;
        {
            Statement rollback(database_,
                "UPDATE wake_intents SET status=?3,terminal_at_ms=?2,"
                "terminal_reason='clock_rollback' "
                "WHERE scope=?1 AND status=?4 AND accepted_at_ms>?2");
            bind_text(database_, rollback.get(), 1, scope);
            sqlite3_bind_int64(rollback.get(), 2, milliseconds(now));
            sqlite3_bind_int(rollback.get(), 3,
                             static_cast<int>(WakeIntentStatus::manual_review));
            sqlite3_bind_int(rollback.get(), 4,
                             static_cast<int>(WakeIntentStatus::scheduled));
            if (sqlite3_step(rollback.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
            result.manual_review = static_cast<std::size_t>(sqlite3_changes(database_));
        }
        {
            Statement due(database_,
                "UPDATE wake_intents SET status=?3,terminal_at_ms=?2,"
                "terminal_reason='' "
                "WHERE scope=?1 AND status=?4 AND due_at_ms<=?2");
            bind_text(database_, due.get(), 1, scope);
            sqlite3_bind_int64(due.get(), 2, milliseconds(now));
            sqlite3_bind_int(due.get(), 3,
                             static_cast<int>(WakeIntentStatus::fired));
            sqlite3_bind_int(due.get(), 4,
                             static_cast<int>(WakeIntentStatus::scheduled));
            if (sqlite3_step(due.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
            result.fired = static_cast<std::size_t>(sqlite3_changes(database_));
        }
        return commit_result(database_, result);
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

WakeIntentRevokeResult WakeIntentStore::revoke(
    const std::string& scope,
    const std::string& id,
    const WakeIntentTimePoint now,
    const std::string& reason)
{
    if (!valid_observation(scope, now)
        || !scheduling::wake::valid_wake_intent_identifier(id)
        || reason.empty()
        || reason.size() > scheduling::wake::wake_intent_reason_max_bytes) {
        return WakeIntentRevokeResult::invalid;
    }

    execute(database_, "BEGIN IMMEDIATE;");
    try {
        const auto intent = find(scope, id);
        if (!intent) {
            return commit_result(database_, WakeIntentRevokeResult::not_found);
        }
        if (scheduling::wake::is_terminal(intent->status)) {
            return commit_result(database_, WakeIntentRevokeResult::terminal);
        }
        if (now < intent->accepted_at) {
            transition(database_, scope, id, WakeIntentStatus::manual_review,
                       now, "clock_rollback");
            return commit_result(database_, WakeIntentRevokeResult::manual_review);
        }
        if (now >= intent->due_at) {
            transition(database_, scope, id, WakeIntentStatus::fired, now, {});
            return commit_result(database_, WakeIntentRevokeResult::fired);
        }
        transition(database_, scope, id, WakeIntentStatus::revoked, now, reason);
        return commit_result(database_, WakeIntentRevokeResult::revoked);
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

} // namespace gaudere::persistence::sqlite
