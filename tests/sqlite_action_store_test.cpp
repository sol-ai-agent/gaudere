#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
using namespace gaudere::scheduling::wake;
using SqliteStore = gaudere::persistence::sqlite::ActionStore;
using namespace std::chrono_literals;

int failures = 0;
void expect(bool condition, const std::string& message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-sqlite-test-" + std::to_string(
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

Action make_action(std::string id, std::string key)
{
    return Action{std::move(id), std::move(key), true, ActionStatus::pending,
                  EffectResult::none, std::nullopt};
}

void test_round_trip()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    auto action = make_action("a", "key");
    action.status = ActionStatus::running;
    action.effect_result = EffectResult::confirmed;
    action.lease = Lease{"worker", TimePoint{} + 1234ms};
    store.save(action);
    const auto loaded = store.find("a");
    expect(loaded && loaded->idempotency_key == "key" && loaded->critical,
           "action round-trips");
    expect(loaded && loaded->lease && loaded->lease->expires_at == TimePoint{} + 1234ms,
           "lease time round-trips as milliseconds");
}

void test_atomic_uniqueness()
{
    TemporaryDatabase database;
    SqliteStore first(database.path.string());
    SqliteStore second(database.path.string());
    std::atomic<int> accepted{0};
    auto write = [&](SqliteStore& store, const std::string& id) {
        try { store.save(make_action(id, "same-key")); ++accepted; }
        catch (const std::exception&) {}
    };
    std::thread a(write, std::ref(first), "a");
    std::thread b(write, std::ref(second), "b");
    a.join(); b.join();
    expect(accepted == 1, "database atomically enforces idempotency key uniqueness");
}

void test_recovery_and_safe()
{
    TemporaryDatabase database;
    const TimePoint now = TimePoint{} + 10s;
    {
        SqliteStore store(database.path.string());
        auto retry = make_action("retry", "retry");
        retry.status = ActionStatus::running;
        retry.lease = Lease{"worker", now - 1s};
        store.save(retry);
        auto unknown = make_action("unknown", "unknown");
        unknown.status = ActionStatus::running;
        unknown.effect_result = EffectResult::unknown;
        unknown.lease = Lease{"worker", now - 1s};
        store.save(unknown);
    }
    SqliteStore reopened(database.path.string());
    Runtime runtime(reopened, [now] { return now; });
    runtime.recover();
    expect(reopened.find("retry")->status == ActionStatus::retry_wait,
           "expired lease survives reopen and becomes retry_wait");
    expect(reopened.find("unknown")->status == ActionStatus::manual_review,
           "unknown effect survives reopen and becomes manual_review");
    runtime.request_shutdown();
    expect(runtime.try_mark_safe(), "recovered runtime can drain to safe");
}
} // namespace

int main()
{
    test_round_trip();
    test_atomic_uniqueness();
    test_recovery_and_safe();
    if (failures) return 1;
    std::cout << "All SQLite action store tests passed\n";
    return 0;
}
