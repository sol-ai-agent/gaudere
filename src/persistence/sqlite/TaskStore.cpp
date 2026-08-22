#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere::persistence::sqlite {
namespace {

using Task = work::Task;
using TaskStatus = work::TaskStatus;
using TaskResult = work::TaskResult;
using Lease = work::Lease;
using TimePoint = work::TimePoint;

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

bool has_column(sqlite3* database, const char* table, const char* column)
{
    const std::string sql = std::string{"PRAGMA table_info("} + table + ")";
    Statement statement(database, sql.c_str());
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return false;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        const auto* name = sqlite3_column_text(statement.get(), 1);
        if (name && std::string(reinterpret_cast<const char*>(name)) == column) {
            return true;
        }
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

Task read_task(sqlite3_stmt* statement)
{
    Task task;
    task.id = text(statement, 0);
    task.idempotency_key = text(statement, 1);
    task.kind = text(statement, 2);
    task.input_content_type = text(statement, 3);
    task.input = text(statement, 4);
    task.limits.max_input_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 5));
    task.limits.max_output_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 6));
    task.limits.max_runtime = std::chrono::milliseconds{
        sqlite3_column_int64(statement, 7)};
    task.limits.max_attempts = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement, 8));
    task.attempts_started = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement, 9));
    task.status = static_cast<TaskStatus>(sqlite3_column_int(statement, 10));
    if (sqlite3_column_type(statement, 11) != SQLITE_NULL) {
        task.lease = Lease{text(statement, 11),
                           time_point(sqlite3_column_int64(statement, 12))};
    }
    task.cancel_reason = text(statement, 13);
    if (sqlite3_column_type(statement, 14) != SQLITE_NULL) {
        task.result = TaskResult{text(statement, 14), text(statement, 15),
                                 text(statement, 16), text(statement, 17),
                                 text(statement, 18), text(statement, 19)};
    }
    return task;
}

constexpr const char* columns =
    "id,idempotency_key,kind,input_content_type,input,"
    "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
    "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
    "result_content_type,result_output,result_failure_code,result_failure_message,"
    "result_metadata_content_type,result_metadata";

} // namespace

TaskStore::TaskStore(const std::string& path)
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
            "CREATE TABLE IF NOT EXISTS tasks ("
            " id TEXT PRIMARY KEY NOT NULL,"
            " idempotency_key TEXT NOT NULL UNIQUE,"
            " kind TEXT NOT NULL,"
            " input_content_type TEXT NOT NULL,"
            " input TEXT NOT NULL,"
            " max_input_bytes INTEGER NOT NULL CHECK(max_input_bytes > 0),"
            " max_output_bytes INTEGER NOT NULL CHECK(max_output_bytes > 0),"
            " max_runtime_ms INTEGER NOT NULL CHECK(max_runtime_ms > 0),"
            " max_attempts INTEGER NOT NULL CHECK(max_attempts > 0 AND max_attempts <= 4294967295),"
            " attempts_started INTEGER NOT NULL CHECK(attempts_started >= 0 AND attempts_started <= max_attempts),"
            " status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 6),"
            " lease_owner TEXT,"
            " lease_expires_at_ms INTEGER,"
            " cancel_reason TEXT NOT NULL,"
            " result_content_type TEXT,"
            " result_output TEXT,"
            " result_failure_code TEXT,"
            " result_failure_message TEXT,"
            " result_metadata_content_type TEXT,"
            " result_metadata TEXT,"
            " CHECK((lease_owner IS NULL) = (lease_expires_at_ms IS NULL)),"
            " CHECK((status IN (1,2)) = (lease_owner IS NOT NULL)),"
            " CHECK((result_content_type IS NULL) = (result_output IS NULL)),"
            " CHECK((result_content_type IS NULL) = (result_failure_code IS NULL)),"
            " CHECK((result_content_type IS NULL) = (result_failure_message IS NULL)),"
            " CHECK((result_metadata_content_type IS NULL) = (result_metadata IS NULL)),"
            " CHECK((status BETWEEN 3 AND 6) = (result_content_type IS NOT NULL))"
            ");");

        // Schema v3 is an additive migration. The column checks make this safe both
        // when ActionStore created a fresh version-1 database before TaskStore and
        // when an existing production v2 tasks table is being upgraded.
        if (!has_column(database_, "tasks", "result_metadata_content_type")) {
            execute(database_,
                "ALTER TABLE tasks ADD COLUMN result_metadata_content_type TEXT;");
        }
        if (!has_column(database_, "tasks", "result_metadata")) {
            execute(database_, "ALTER TABLE tasks ADD COLUMN result_metadata TEXT;");
        }
        if (version < 3) {
            execute(database_, "PRAGMA user_version=3;");
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

TaskStore::~TaskStore()
{
    sqlite3_close(database_);
}

std::optional<Task> TaskStore::find(const std::string& id) const
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM tasks WHERE id=?1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        return read_task(statement.get());
    }
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    throw std::runtime_error(sqlite3_errmsg(database_));
}

std::optional<Task> TaskStore::find_by_idempotency_key(const std::string& key) const
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM tasks WHERE idempotency_key=?1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, key);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        return read_task(statement.get());
    }
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    throw std::runtime_error(sqlite3_errmsg(database_));
}

std::optional<Task> TaskStore::find_pending_for(
    const std::vector<std::string>& accepted_kinds) const
{
    if (accepted_kinds.empty()) {
        return std::nullopt;
    }

    std::string sql = std::string{"SELECT "} + columns
        + " FROM tasks WHERE status=? AND kind IN (";
    for (std::size_t index = 0; index < accepted_kinds.size(); ++index) {
        if (index != 0) {
            sql += ',';
        }
        sql += '?';
    }
    sql += ") ORDER BY rowid ASC LIMIT 1";

    Statement statement(database_, sql.c_str());
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(TaskStatus::pending));
    for (std::size_t index = 0; index < accepted_kinds.size(); ++index) {
        bind_text(database_, statement.get(), static_cast<int>(index + 2),
                  accepted_kinds[index]);
    }

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        return read_task(statement.get());
    }
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    throw std::runtime_error(sqlite3_errmsg(database_));
}

std::vector<Task> TaskStore::leased_with_expired_lease(const TimePoint now) const
{
    const std::string sql = std::string{"SELECT "} + columns
        + " FROM tasks WHERE status IN (?1,?2) AND lease_expires_at_ms<=?3";
    Statement statement(database_, sql.c_str());
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(TaskStatus::running));
    sqlite3_bind_int(statement.get(), 2, static_cast<int>(TaskStatus::cancel_requested));
    sqlite3_bind_int64(statement.get(), 3, milliseconds(now));

    std::vector<Task> tasks;
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_ROW) {
            tasks.push_back(read_task(statement.get()));
        } else if (result == SQLITE_DONE) {
            return tasks;
        } else {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }
}

std::optional<TimePoint> TaskStore::next_lease_expiry() const
{
    Statement statement(database_,
        "SELECT MIN(lease_expires_at_ms) FROM tasks WHERE status IN (?1,?2)");
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(TaskStatus::running));
    sqlite3_bind_int(statement.get(), 2, static_cast<int>(TaskStatus::cancel_requested));

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL) {
        return std::nullopt;
    }
    return time_point(sqlite3_column_int64(statement.get(), 0));
}

bool TaskStore::has_active() const
{
    Statement statement(database_,
        "SELECT 1 FROM tasks WHERE status IN (?1,?2) LIMIT 1");
    sqlite3_bind_int(statement.get(), 1, static_cast<int>(TaskStatus::running));
    sqlite3_bind_int(statement.get(), 2, static_cast<int>(TaskStatus::cancel_requested));
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error(sqlite3_errmsg(database_));
}

void TaskStore::save(const Task& task)
{
    Statement statement(database_,
        "INSERT INTO tasks ("
        "id,idempotency_key,kind,input_content_type,input,"
        "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
        "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
        "result_content_type,result_output,result_failure_code,result_failure_message,"
        "result_metadata_content_type,result_metadata"
        ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20) "
        "ON CONFLICT(id) DO UPDATE SET "
        "idempotency_key=excluded.idempotency_key,kind=excluded.kind,"
        "input_content_type=excluded.input_content_type,input=excluded.input,"
        "max_input_bytes=excluded.max_input_bytes,max_output_bytes=excluded.max_output_bytes,"
        "max_runtime_ms=excluded.max_runtime_ms,max_attempts=excluded.max_attempts,"
        "attempts_started=excluded.attempts_started,status=excluded.status,"
        "lease_owner=excluded.lease_owner,lease_expires_at_ms=excluded.lease_expires_at_ms,"
        "cancel_reason=excluded.cancel_reason,result_content_type=excluded.result_content_type,"
        "result_output=excluded.result_output,result_failure_code=excluded.result_failure_code,"
        "result_failure_message=excluded.result_failure_message,"
        "result_metadata_content_type=excluded.result_metadata_content_type,"
        "result_metadata=excluded.result_metadata");

    bind_text(database_, statement.get(), 1, task.id);
    bind_text(database_, statement.get(), 2, task.idempotency_key);
    bind_text(database_, statement.get(), 3, task.kind);
    bind_text(database_, statement.get(), 4, task.input_content_type);
    bind_text(database_, statement.get(), 5, task.input);
    sqlite3_bind_int64(statement.get(), 6,
                       static_cast<sqlite3_int64>(task.limits.max_input_bytes));
    sqlite3_bind_int64(statement.get(), 7,
                       static_cast<sqlite3_int64>(task.limits.max_output_bytes));
    sqlite3_bind_int64(statement.get(), 8, task.limits.max_runtime.count());
    sqlite3_bind_int64(statement.get(), 9, task.limits.max_attempts);
    sqlite3_bind_int64(statement.get(), 10, task.attempts_started);
    sqlite3_bind_int(statement.get(), 11, static_cast<int>(task.status));

    if (task.lease) {
        bind_text(database_, statement.get(), 12, task.lease->owner);
        sqlite3_bind_int64(statement.get(), 13, milliseconds(task.lease->expires_at));
    } else {
        sqlite3_bind_null(statement.get(), 12);
        sqlite3_bind_null(statement.get(), 13);
    }

    bind_text(database_, statement.get(), 14, task.cancel_reason);
    if (task.result) {
        bind_text(database_, statement.get(), 15, task.result->content_type);
        bind_text(database_, statement.get(), 16, task.result->output);
        bind_text(database_, statement.get(), 17, task.result->failure_code);
        bind_text(database_, statement.get(), 18, task.result->failure_message);
        if (!task.result->metadata_content_type.empty() || !task.result->metadata.empty()) {
            bind_text(database_, statement.get(), 19,
                      task.result->metadata_content_type);
            bind_text(database_, statement.get(), 20, task.result->metadata);
        } else {
            sqlite3_bind_null(statement.get(), 19);
            sqlite3_bind_null(statement.get(), 20);
        }
    } else {
        sqlite3_bind_null(statement.get(), 15);
        sqlite3_bind_null(statement.get(), 16);
        sqlite3_bind_null(statement.get(), 17);
        sqlite3_bind_null(statement.get(), 18);
        sqlite3_bind_null(statement.get(), 19);
        sqlite3_bind_null(statement.get(), 20);
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
}

} // namespace gaudere::persistence::sqlite
