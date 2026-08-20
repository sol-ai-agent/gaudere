#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace gaudere::work;
using SqliteStore = gaudere::persistence::sqlite::TaskStore;
using ActionSqliteStore = gaudere::persistence::sqlite::ActionStore;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-task-sqlite-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    }

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

Task make_task(std::string id, std::string key)
{
    Task task;
    task.id = std::move(id);
    task.idempotency_key = std::move(key);
    task.kind = "test.echo";
    task.input_content_type = "application/json";
    task.input = "{\"text\":\"hello\"}";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 2048;
    task.limits.max_runtime = 30s;
    task.limits.max_attempts = 3;
    return task;
}

void test_round_trip()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    auto task = make_task("a", "key");
    task.status = TaskStatus::running;
    task.attempts_started = 1;
    task.lease = Lease{"worker", TimePoint{} + 1234ms};
    store.save(task);

    const auto loaded = store.find("a");
    expect(loaded && loaded->kind == "test.echo"
               && loaded->input_content_type == "application/json"
               && loaded->limits.max_runtime == 30s
               && loaded->limits.max_attempts == 3,
           "task definition round-trips");
    expect(loaded && loaded->lease
               && loaded->lease->expires_at == TimePoint{} + 1234ms,
           "task lease time round-trips as milliseconds");

    task.status = TaskStatus::succeeded;
    task.lease.reset();
    task.result = TaskResult{"application/json", "{\"ok\":true}", {}, {},
                             "application/json", "{\"tokens\":7}"};
    store.save(task);
    const auto completed = store.find("a");
    expect(completed && completed->result
               && completed->result->output == "{\"ok\":true}"
               && completed->result->metadata_content_type == "application/json"
               && completed->result->metadata == "{\"tokens\":7}",
           "task result and structured metadata round-trip atomically");
}

void test_v2_metadata_migration()
{
    TemporaryDatabase database;
    sqlite3* raw = nullptr;
    expect(sqlite3_open(database.path.c_str(), &raw) == SQLITE_OK,
           "legacy v2 test database opens");
    if (!raw) {
        return;
    }
    const char* schema =
        "CREATE TABLE tasks ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "idempotency_key TEXT NOT NULL UNIQUE,"
        "kind TEXT NOT NULL,"
        "input_content_type TEXT NOT NULL,"
        "input TEXT NOT NULL,"
        "max_input_bytes INTEGER NOT NULL,"
        "max_output_bytes INTEGER NOT NULL,"
        "max_runtime_ms INTEGER NOT NULL,"
        "max_attempts INTEGER NOT NULL,"
        "attempts_started INTEGER NOT NULL,"
        "status INTEGER NOT NULL,"
        "lease_owner TEXT,"
        "lease_expires_at_ms INTEGER,"
        "cancel_reason TEXT NOT NULL,"
        "result_content_type TEXT,"
        "result_output TEXT,"
        "result_failure_code TEXT,"
        "result_failure_message TEXT"
        ");"
        "PRAGMA user_version=2;";
    char* error = nullptr;
    if (sqlite3_exec(raw, schema, nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << "FAIL: create legacy v2 schema: "
                  << (error ? error : sqlite3_errmsg(raw)) << '\n';
        ++failures;
        sqlite3_free(error);
        sqlite3_close(raw);
        return;
    }
    sqlite3_close(raw);

    {
        SqliteStore migrated(database.path.string());
        auto task = make_task("migrated", "migrated");
        migrated.save(task);
        Runtime runtime(migrated, [] { return TimePoint{} + 1s; });
        runtime.recover();
        expect(runtime.start("migrated", "worker"),
               "migrated v2 task can start");
        expect(runtime.succeed("migrated", "ok", "text/plain",
                               "application/json", "{\"total_tokens\":9}")
                   == FinishResult::accepted,
               "migrated v2 task accepts structured metadata");
    }

    SqliteStore reopened(database.path.string());
    const auto task = reopened.find("migrated");
    expect(task && task->result
               && task->result->metadata_content_type == "application/json"
               && task->result->metadata == "{\"total_tokens\":9}",
           "v2 to v3 metadata migration survives reopen");

    sqlite3* check = nullptr;
    expect(sqlite3_open(database.path.c_str(), &check) == SQLITE_OK,
           "migrated database reopens for version check");
    if (check) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(check, "PRAGMA user_version", -1,
                               &statement, nullptr) == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_ROW) {
            expect(sqlite3_column_int(statement, 0) == 3,
                   "metadata migration advances SQLite user_version to 3");
        } else {
            expect(false, "read migrated SQLite user_version");
        }
        sqlite3_finalize(statement);
        sqlite3_close(check);
    }
}

void test_pending_selection()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());

    auto unsupported = make_task("unsupported", "unsupported");
    unsupported.kind = "provider.missing";
    store.save(unsupported);

    auto first = make_task("z-first", "z-first");
    first.kind = "local.echo";
    store.save(first);

    auto second = make_task("a-second", "a-second");
    second.kind = "local.echo";
    store.save(second);

    expect(!store.find_pending_for({}),
           "empty accepted kind set selects no SQLite task");
    const auto selected = store.find_pending_for({"local.echo"});
    expect(selected && selected->id == "z-first",
           "SQLite selection skips unsupported kinds and preserves insertion order");

    first.status = TaskStatus::running;
    first.attempts_started = 1;
    first.lease = Lease{"worker", TimePoint{} + 30s};
    store.save(first);
    const auto next = store.find_pending_for({"local.echo"});
    expect(next && next->id == "a-second",
           "SQLite selection skips an already active task");
}

void test_next_lease_expiry()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    expect(!store.next_lease_expiry(),
           "empty SQLite task store has no recovery deadline");

    auto later = make_task("later", "later");
    later.status = TaskStatus::running;
    later.attempts_started = 1;
    later.lease = Lease{"worker-a", TimePoint{} + 20s};
    store.save(later);

    auto earlier = make_task("earlier", "earlier");
    earlier.status = TaskStatus::cancel_requested;
    earlier.attempts_started = 1;
    earlier.cancel_reason = "shutdown";
    earlier.lease = Lease{"worker-b", TimePoint{} + 10s};
    store.save(earlier);

    expect(store.next_lease_expiry() == TimePoint{} + 10s,
           "SQLite exposes the earliest active lease expiry");

    earlier.status = TaskStatus::cancelled;
    earlier.lease.reset();
    earlier.result = TaskResult{"text/plain", {}, "cancelled", "shutdown"};
    store.save(earlier);
    expect(store.next_lease_expiry() == TimePoint{} + 20s,
           "terminal work is removed from the recovery deadline");
}

void test_atomic_uniqueness()
{
    TemporaryDatabase database;
    SqliteStore first(database.path.string());
    SqliteStore second(database.path.string());
    std::atomic<int> accepted{0};

    auto write = [&](SqliteStore& store, const std::string& id) {
        try {
            store.save(make_task(id, "same-key"));
            ++accepted;
        } catch (const std::exception&) {
        }
    };

    std::thread a(write, std::ref(first), "a");
    std::thread b(write, std::ref(second), "b");
    a.join();
    b.join();
    expect(accepted == 1,
           "database atomically enforces task idempotency key uniqueness");
}

void test_recovery_after_reopen()
{
    TemporaryDatabase database;
    const TimePoint now = TimePoint{} + 1min;
    {
        SqliteStore store(database.path.string());
        auto retry = make_task("retry", "retry");
        retry.status = TaskStatus::running;
        retry.attempts_started = 1;
        retry.lease = Lease{"worker", now - 1s};
        store.save(retry);

        auto cancellation = make_task("cancel", "cancel");
        cancellation.status = TaskStatus::cancel_requested;
        cancellation.attempts_started = 1;
        cancellation.cancel_reason = "operator request";
        cancellation.lease = Lease{"worker", now - 1s};
        store.save(cancellation);
    }

    SqliteStore reopened(database.path.string());
    Runtime runtime(reopened, [now] { return now; });
    runtime.recover();
    expect(reopened.find("retry")->status == TaskStatus::pending,
           "expired task lease survives reopen and returns to pending");
    expect(reopened.find("cancel")->status == TaskStatus::cancelled,
           "cancellation survives reopen and completes safely");
}

void test_shared_schema_with_action_store()
{
    TemporaryDatabase database;
    {
        ActionSqliteStore actions(database.path.string());
        SqliteStore tasks(database.path.string());
    }
    try {
        ActionSqliteStore actions(database.path.string());
        SqliteStore tasks(database.path.string());
        expect(true, "action and task stores share the versioned SQLite state");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: shared schema reopen: " << error.what() << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    test_round_trip();
    test_v2_metadata_migration();
    test_pending_selection();
    test_next_lease_expiry();
    test_atomic_uniqueness();
    test_recovery_after_reopen();
    test_shared_schema_with_action_store();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All SQLite task store tests passed\n";
    return 0;
}
