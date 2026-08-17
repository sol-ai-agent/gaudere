#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

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
           "task lease round-trips as milliseconds");

    task.status = TaskStatus::succeeded;
    task.lease.reset();
    task.result = TaskResult{"application/json", "{\"ok\":true}", {}, {}};
    store.save(task);
    const auto completed = store.find("a");
    expect(completed && completed->result
               && completed->result->output == "{\"ok\":true}",
           "task result round-trips atomically with task state");
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
    test_pending_selection();
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
