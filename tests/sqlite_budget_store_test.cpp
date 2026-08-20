#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using gaudere::budget::ConsumeResult;
using gaudere::budget::Policy;
using gaudere::budget::TimePoint;
using SqliteStore = gaudere::persistence::sqlite::BudgetStore;
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
            / ("gaudere-budget-test-" + std::to_string(
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

Policy policy()
{
    Policy result;
    result.max_total = 4;
    result.max_in_window = 2;
    result.window = 24h;
    result.min_interval = 15min;
    return result;
}

void test_limits_and_idempotency()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    const auto start = TimePoint{} + 100h;
    const auto limits = policy();

    expect(store.consume("provider", "one", start, limits) == ConsumeResult::accepted,
           "first permit is accepted");
    expect(store.consume("provider", "one", start + 1min, limits) == ConsumeResult::duplicate,
           "same idempotency key remains accepted as a duplicate");
    expect(store.consume("provider", "two", start + 10min, limits) == ConsumeResult::cooldown,
           "minimum interval is enforced");
    expect(store.consume("provider", "two", start + 15min, limits) == ConsumeResult::accepted,
           "permit is accepted exactly at the minimum interval");
    expect(store.consume("provider", "three", start + 30min, limits)
               == ConsumeResult::window_exhausted,
           "rolling window limit is enforced");

    expect(store.consume("provider", "three", start + 24h + 1ms, limits)
               == ConsumeResult::accepted,
           "oldest permit expires from the rolling window");
    expect(store.consume("provider", "four", start + 24h + 16min, limits)
               == ConsumeResult::accepted,
           "fourth lifetime permit is accepted after cooldown");
    expect(store.consume("provider", "five", start + 48h, limits)
               == ConsumeResult::total_exhausted,
           "lifetime limit is enforced even after the rolling window expires");

    expect(store.consume("other-provider", "one", start, limits) == ConsumeResult::accepted,
           "budget scopes are independent");
}

void test_snapshot_is_observational()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    const auto start = TimePoint{} + 150h;
    const auto limits = policy();

    const auto empty = store.snapshot("provider", start, limits);
    expect(empty.total_used == 0 && empty.in_window_used == 0
               && !empty.last_consumed_at
               && empty.next_new_consumption == ConsumeResult::accepted,
           "empty snapshot reports an available untouched budget");

    expect(store.consume("provider", "one", start, limits) == ConsumeResult::accepted,
           "snapshot setup permit one accepted");
    const auto cooldown = store.snapshot("provider", start + 5min, limits);
    expect(cooldown.total_used == 1 && cooldown.in_window_used == 1
               && cooldown.last_consumed_at == start
               && cooldown.next_new_consumption == ConsumeResult::cooldown,
           "snapshot reports durable usage and cooldown");

    const auto ready = store.snapshot("provider", start + 15min, limits);
    expect(ready.next_new_consumption == ConsumeResult::accepted,
           "snapshot reports readiness exactly at cooldown boundary");
    expect(store.consume("provider", "two", start + 15min, limits)
               == ConsumeResult::accepted,
           "snapshot never consumes the observed permit");

    const auto window_full = store.snapshot("provider", start + 30min, limits);
    expect(window_full.total_used == 2 && window_full.in_window_used == 2
               && window_full.last_consumed_at == start + 15min
               && window_full.next_new_consumption == ConsumeResult::window_exhausted,
           "snapshot reports rolling-window exhaustion after cooldown");

    const auto window_reopened = store.snapshot("provider", start + 24h + 1ms, limits);
    expect(window_reopened.total_used == 2 && window_reopened.in_window_used == 1
               && window_reopened.next_new_consumption == ConsumeResult::accepted,
           "snapshot reports when an old permit leaves the rolling window");

    const auto rollback = store.snapshot("provider", start - 1ms, limits);
    expect(rollback.next_new_consumption == ConsumeResult::clock_rollback,
           "snapshot fails closed when wall clock precedes latest consumption");
}

void test_clock_rollback_fails_closed()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    const auto start = TimePoint{} + 200h;
    const auto limits = policy();

    expect(store.consume("provider", "one", start, limits) == ConsumeResult::accepted,
           "clock rollback setup permit accepted");
    expect(store.consume("provider", "two", start - 1ms, limits)
               == ConsumeResult::clock_rollback,
           "wall-clock rollback fails closed");
}

void test_reopen_preserves_consumption()
{
    TemporaryDatabase database;
    const auto start = TimePoint{} + 300h;
    const auto limits = policy();

    {
        SqliteStore store(database.path.string());
        expect(store.consume("provider", "one", start, limits) == ConsumeResult::accepted,
               "permit accepted before reopen");
    }

    SqliteStore reopened(database.path.string());
    expect(reopened.consume("provider", "one", start + 1min, limits)
               == ConsumeResult::duplicate,
           "idempotent permit survives reopen");
    expect(reopened.consume("provider", "two", start + 1min, limits)
               == ConsumeResult::cooldown,
           "cooldown survives reopen");
    const auto snapshot = reopened.snapshot("provider", start + 1min, limits);
    expect(snapshot.total_used == 1 && snapshot.next_new_consumption == ConsumeResult::cooldown,
           "snapshot observes persisted consumption after reopen");
}

void test_atomic_total_limit()
{
    TemporaryDatabase database;
    SqliteStore first(database.path.string());
    SqliteStore second(database.path.string());

    Policy limits;
    limits.max_total = 1;
    limits.max_in_window = 1;
    limits.window = 24h;
    limits.min_interval = 0ms;

    const auto now = TimePoint{} + 400h;
    std::atomic<int> accepted{0};
    std::atomic<int> exhausted{0};

    auto consume = [&](SqliteStore& store, const std::string& key) {
        const auto result = store.consume("provider", key, now, limits);
        if (result == ConsumeResult::accepted) {
            ++accepted;
        } else if (result == ConsumeResult::total_exhausted) {
            ++exhausted;
        }
    };

    std::thread a(consume, std::ref(first), "a");
    std::thread b(consume, std::ref(second), "b");
    a.join();
    b.join();

    expect(accepted == 1, "only one concurrent consumer obtains the final permit");
    expect(exhausted == 1, "the competing consumer observes total exhaustion");
}

void test_invalid_policy_is_rejected()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Policy invalid;
    invalid.max_total = 1;
    invalid.max_in_window = 2;
    invalid.window = 24h;

    bool consume_threw = false;
    try {
        static_cast<void>(store.consume("provider", "one", TimePoint{}, invalid));
    } catch (const std::invalid_argument&) {
        consume_threw = true;
    }
    expect(consume_threw, "invalid policy is rejected before persistence");

    bool snapshot_threw = false;
    try {
        static_cast<void>(store.snapshot("provider", TimePoint{}, invalid));
    } catch (const std::invalid_argument&) {
        snapshot_threw = true;
    }
    expect(snapshot_threw, "invalid policy is rejected before observation");
}

} // namespace

int main()
{
    test_limits_and_idempotency();
    test_snapshot_is_observational();
    test_clock_rollback_fails_closed();
    test_reopen_preserves_consumption();
    test_atomic_total_limit();
    test_invalid_policy_is_rejected();

    if (failures != 0) {
        return 1;
    }
    std::cout << "All SQLite budget store tests passed\n";
    return 0;
}
