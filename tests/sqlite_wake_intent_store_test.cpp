#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntent.hpp>
#include <gaudere/work/Task.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace gaudere::scheduling::wake;
using SqliteStore = gaudere::persistence::sqlite::WakeIntentStore;
using namespace std::chrono_literals;

int failures = 0;
std::atomic<unsigned long long> database_counter{0};

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    explicit TemporaryDatabase(std::string label = "wake-intent")
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-" + std::move(label) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + "-" + std::to_string(database_counter.fetch_add(1)) + ".db");
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

WakeIntent intent(std::string scope,
                  std::string id,
                  std::string source_id,
                  const WakeIntentTimePoint accepted_at,
                  const WakeIntentTimePoint due_at)
{
    WakeIntent result;
    result.scope = std::move(scope);
    result.id = std::move(id);
    result.source_id = std::move(source_id);
    result.accepted_at = accepted_at;
    result.due_at = due_at;
    return result;
}

int user_version(const std::filesystem::path& path)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        return -1;
    }
    sqlite3_stmt* statement = nullptr;
    int version = -1;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1,
                           &statement, nullptr) == SQLITE_OK
        && sqlite3_step(statement) == SQLITE_ROW) {
        version = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return version;
}

bool execute_sql(const std::filesystem::path& path, const char* sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (error) {
        sqlite3_free(error);
    }
    sqlite3_close(database);
    return result == SQLITE_OK;
}

gaudere::work::Task task(std::string id)
{
    gaudere::work::Task result;
    result.id = id;
    result.idempotency_key = "test:" + id;
    result.kind = "test.echo";
    result.input_content_type = "text/plain";
    result.input = "hello";
    result.limits.max_input_bytes = 64;
    result.limits.max_output_bytes = 64;
    result.limits.max_runtime = 1s;
    result.limits.max_attempts = 1;
    return result;
}

void test_atomic_acceptance_and_scope_policy()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    const auto accepted_at = WakeIntentTimePoint{100s};
    const auto first = intent("scope", "wake-a", "source-a",
                              accepted_at, accepted_at + 15min);
    const WakeIntentPolicy one{1};

    expect(store.accept(first, one) == WakeIntentAcceptResult::accepted,
           "first durable wake is accepted");
    const auto loaded = store.find("scope", "wake-a");
    expect(loaded && loaded->scope == "scope" && loaded->id == "wake-a"
               && loaded->source_id == "source-a"
               && loaded->accepted_at == accepted_at
               && loaded->due_at == accepted_at + 15min
               && loaded->status == WakeIntentStatus::scheduled
               && !loaded->terminal_at && loaded->terminal_reason.empty(),
           "accepted wake round-trips exactly");

    expect(!execute_sql(database.path,
                        "UPDATE wake_intents SET due_at_ms=due_at_ms+1 "
                        "WHERE scope='scope' AND id='wake-a'"),
           "SQLite prevents mutation of an accepted deadline");
    expect(!execute_sql(database.path,
                        "DELETE FROM wake_intents "
                        "WHERE scope='scope' AND id='wake-a'"),
           "SQLite prevents recycling an accepted lifetime slot");
    expect(!execute_sql(database.path,
                        "INSERT INTO wake_intents "
                        "(scope,id,source_id,accepted_at_ms,due_at_ms,status,"
                        "terminal_at_ms,terminal_reason) "
                        "VALUES ('scope','forged','forged-source',0,1,1,1,'')"),
           "SQLite prevents bypassing acceptance with a terminal insert");
    expect(store.find("scope", "wake-a")->due_at == accepted_at + 15min,
           "rejected direct mutations leave the accepted row unchanged");
    const auto by_source = store.find_by_source("scope", "source-a");
    expect(by_source && loaded && by_source->id == loaded->id
               && by_source->due_at == loaded->due_at,
           "source identity lookup returns the same durable row");

    const auto duplicate = intent("scope", "different-id", "source-a",
                                  accepted_at + 1s, accepted_at + 20min);
    expect(store.accept(duplicate, one) == WakeIntentAcceptResult::duplicate,
           "same source is idempotent before policy exhaustion");
    expect(store.find("scope", "wake-a")->due_at == accepted_at + 15min,
           "duplicate never replaces the original deadline");

    const auto conflict = intent("scope", "wake-a", "different-source",
                                 accepted_at, accepted_at + 20min);
    expect(store.accept(conflict, one) == WakeIntentAcceptResult::conflict,
           "reused wake identity with another source is a conflict");

    const auto exhausted = intent("scope", "wake-b", "source-b",
                                  accepted_at, accepted_at + 20min);
    expect(store.accept(exhausted, one)
               == WakeIntentAcceptResult::total_exhausted,
           "per-scope lifetime total is hard");

    auto other_scope = exhausted;
    other_scope.scope = "other.scope";
    expect(store.accept(other_scope, one) == WakeIntentAcceptResult::accepted,
           "core policy scopes are independent");

    auto invalid = exhausted;
    invalid.scope.clear();
    expect(store.accept(invalid, one) == WakeIntentAcceptResult::invalid,
           "invalid definition reaches no SQLite write");
}

void test_exact_due_reconciliation_survives_reopen()
{
    TemporaryDatabase database;
    const auto accepted_at = WakeIntentTimePoint{200s};
    const WakeIntentPolicy policy{3};

    {
        SqliteStore store(database.path.string());
        expect(store.accept(intent("scope", "early", "source-early",
                                  accepted_at, accepted_at + 10s), policy)
                   == WakeIntentAcceptResult::accepted,
               "early deadline accepted");
        expect(store.accept(intent("scope", "later", "source-later",
                                  accepted_at, accepted_at + 20s), policy)
                   == WakeIntentAcceptResult::accepted,
               "later deadline accepted");
        expect(store.next_scheduled_at("scope") == accepted_at + 10s,
               "SQLite exposes the earliest exact deadline");

        const auto before = store.reconcile("scope", accepted_at + 10s - 1ms);
        expect(before.fired == 0 && before.manual_review == 0,
               "wake does not fire before its deadline");
        const auto exact = store.reconcile("scope", accepted_at + 10s);
        expect(exact.fired == 1 && exact.manual_review == 0,
               "wake fires exactly at the inclusive deadline");
        const auto fired = store.find("scope", "early");
        expect(fired && fired->status == WakeIntentStatus::fired
                   && fired->terminal_at == accepted_at + 10s
                   && fired->terminal_reason.empty(),
               "fired state and observation time are durable");
    }

    {
        SqliteStore reopened(database.path.string());
        expect(reopened.reconcile("scope", accepted_at + 10s).fired == 0,
               "reopen never repeats an already-fired wake");
        expect(reopened.next_scheduled_at("scope") == accepted_at + 20s,
               "reopen re-arms the remaining exact deadline");
        const auto overdue = reopened.reconcile("scope", accepted_at + 21s);
        expect(overdue.fired == 1,
               "overdue wake fires once at the first safe observation");
        const auto later = reopened.find("scope", "later");
        expect(later && later->status == WakeIntentStatus::fired
                   && later->terminal_at == accepted_at + 21s,
               "overdue lateness remains observable");
        expect(!reopened.next_scheduled_at("scope"),
               "terminal rows schedule no future wake");
    }
}

void test_revocation_is_permanent_and_due_wins()
{
    const auto accepted_at = WakeIntentTimePoint{300s};
    const WakeIntentPolicy one{1};

    {
        TemporaryDatabase database("wake-revoke");
        SqliteStore store(database.path.string());
        const auto value = intent("scope", "wake", "source",
                                  accepted_at, accepted_at + 10s);
        expect(store.accept(value, one) == WakeIntentAcceptResult::accepted,
               "revocation fixture accepted");
        expect(store.revoke("scope", "wake", accepted_at + 9s,
                            "operator request") == WakeIntentRevokeResult::revoked,
               "revocation succeeds strictly before due");
        const auto revoked = store.find("scope", "wake");
        expect(revoked && revoked->status == WakeIntentStatus::revoked
                   && revoked->terminal_at == accepted_at + 9s
                   && revoked->terminal_reason == "operator request",
               "revocation reason is durable");
        expect(store.reconcile("scope", accepted_at + 20s).fired == 0,
               "revoked wake never fires");
        expect(store.accept(value, one) == WakeIntentAcceptResult::duplicate,
               "revoked source remains idempotent");
        expect(store.accept(intent("scope", "second", "second-source",
                                   accepted_at, accepted_at + 20s), one)
                   == WakeIntentAcceptResult::total_exhausted,
               "revocation does not refund the lifetime slot");
    }

    {
        TemporaryDatabase database("wake-due-order");
        SqliteStore store(database.path.string());
        expect(store.accept(intent("scope", "wake", "source",
                                   accepted_at, accepted_at + 10s), one)
                   == WakeIntentAcceptResult::accepted,
               "due-order fixture accepted");
        expect(store.revoke("scope", "wake", accepted_at + 10s, "too late")
                   == WakeIntentRevokeResult::fired,
               "firing wins at the exact deadline");
        const auto fired = store.find("scope", "wake");
        expect(fired && fired->status == WakeIntentStatus::fired
                   && fired->terminal_reason.empty(),
               "late revoke persists fired rather than revoked");
        expect(store.revoke("scope", "wake", accepted_at + 11s, "again")
                   == WakeIntentRevokeResult::terminal,
               "terminal record cannot be changed");
        expect(store.revoke("scope", "missing", accepted_at, "reason")
                   == WakeIntentRevokeResult::not_found,
               "missing wake is reported without insertion");
    }
}

void test_clock_rollback_enters_manual_review()
{
    const auto accepted_at = WakeIntentTimePoint{400s};
    const WakeIntentPolicy policy{2};
    TemporaryDatabase database;
    SqliteStore store(database.path.string());

    expect(store.accept(intent("scope", "reconcile", "source-reconcile",
                               accepted_at, accepted_at + 10s), policy)
               == WakeIntentAcceptResult::accepted,
           "rollback reconciliation fixture accepted");
    const auto rollback = store.reconcile("scope", accepted_at - 1ms);
    expect(rollback.manual_review == 1 && rollback.fired == 0,
           "reconciliation detects wall-clock rollback");
    const auto reviewed = store.find("scope", "reconcile");
    expect(reviewed && reviewed->status == WakeIntentStatus::manual_review
               && reviewed->terminal_at == accepted_at - 1ms
               && reviewed->terminal_reason == "clock_rollback",
           "rollback becomes durable manual review");

    expect(store.accept(intent("scope", "revoke", "source-revoke",
                               accepted_at, accepted_at + 20s), policy)
               == WakeIntentAcceptResult::accepted,
           "rollback revocation fixture accepted");
    expect(store.revoke("scope", "revoke", accepted_at - 1ms, "operator")
               == WakeIntentRevokeResult::manual_review,
           "revoke path also fails closed on rollback");
    expect(store.find("scope", "revoke")->status
               == WakeIntentStatus::manual_review,
           "revoke rollback never becomes revoked or fired");
}

void test_atomic_lifetime_limit_across_connections()
{
    TemporaryDatabase database;
    SqliteStore first(database.path.string());
    SqliteStore second(database.path.string());
    const WakeIntentPolicy one{1};
    const auto accepted_at = WakeIntentTimePoint{500s};
    std::atomic<int> accepted{0};
    std::atomic<int> exhausted{0};
    std::atomic<int> errors{0};

    auto consume = [&](SqliteStore& store, const std::string& id) {
        try {
            const auto result = store.accept(
                intent("scope", id, "source-" + id,
                       accepted_at, accepted_at + 10s), one);
            if (result == WakeIntentAcceptResult::accepted) {
                ++accepted;
            } else if (result == WakeIntentAcceptResult::total_exhausted) {
                ++exhausted;
            } else {
                ++errors;
            }
        } catch (...) {
            ++errors;
        }
    };

    std::thread a(consume, std::ref(first), "a");
    std::thread b(consume, std::ref(second), "b");
    a.join();
    b.join();

    expect(accepted == 1, "only one connection obtains the lifetime slot");
    expect(exhausted == 1, "competing connection observes total exhaustion");
    expect(errors == 0, "atomic acceptance has no transaction error");
}

void test_schema_v3_migrates_additively_to_v4()
{
    TemporaryDatabase database("wake-migration");
    gaudere::budget::Policy budget_policy;
    budget_policy.max_total = 2;
    budget_policy.max_in_window = 2;
    budget_policy.window = 24h;
    budget_policy.min_interval = 0ms;
    {
        gaudere::persistence::sqlite::ActionStore actions(database.path.string());
        gaudere::scheduling::wake::Action historical_action;
        historical_action.id = "historical-action";
        historical_action.idempotency_key = "historical-action-key";
        historical_action.status = gaudere::scheduling::wake::ActionStatus::succeeded;
        historical_action.effect_result =
            gaudere::scheduling::wake::EffectResult::confirmed;
        actions.save(historical_action);
        gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
        tasks.save(task("historical"));
        gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
        expect(budgets.consume("test", "historical-permit",
                               gaudere::budget::TimePoint{700s}, budget_policy)
                   == gaudere::budget::ConsumeResult::accepted,
               "historical budget permit exists before migration");
    }
    expect(user_version(database.path) == 3,
           "existing action/task state starts at schema v3");

    {
        SqliteStore wakes(database.path.string());
        expect(wakes.accept(intent("scope", "wake", "source",
                                   WakeIntentTimePoint{600s},
                                   WakeIntentTimePoint{610s}), {1})
                   == WakeIntentAcceptResult::accepted,
               "v3 to v4 migration immediately supports wake state");
    }
    expect(user_version(database.path) == 4,
           "wake store advances SQLite user_version to 4");

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::ActionStore actions(database.path.string());
    gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
    expect(tasks.find("historical").has_value(),
           "v3 historical task survives additive migration");
    expect(actions.find("historical-action").has_value(),
           "v3 historical action survives additive migration");
    const auto budget = budgets.snapshot(
        "test", gaudere::budget::TimePoint{700s}, budget_policy);
    expect(budget.total_used == 1,
           "historical budget consumption survives additive migration");
}

enum class StoreKind { action, budget, task, wake };

void construct(const StoreKind kind, const std::filesystem::path& path)
{
    switch (kind) {
    case StoreKind::action: {
        gaudere::persistence::sqlite::ActionStore store(path.string());
        return;
    }
    case StoreKind::budget: {
        gaudere::persistence::sqlite::BudgetStore store(path.string());
        return;
    }
    case StoreKind::task: {
        gaudere::persistence::sqlite::TaskStore store(path.string());
        return;
    }
    case StoreKind::wake: {
        SqliteStore store(path.string());
        return;
    }
    }
}

void test_all_store_construction_orders_accept_v4()
{
    std::array<StoreKind, 4> order{{StoreKind::action, StoreKind::budget,
                                   StoreKind::task, StoreKind::wake}};
    std::size_t index = 0;
    do {
        TemporaryDatabase database("wake-order-" + std::to_string(index));
        bool constructed = true;
        try {
            for (const auto kind : order) {
                construct(kind, database.path);
            }
            gaudere::persistence::sqlite::ActionStore actions(database.path.string());
            gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
            gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
            SqliteStore wakes(database.path.string());
        } catch (...) {
            constructed = false;
        }
        expect(constructed,
               "all SQLite stores reopen after construction order "
                   + std::to_string(index));
        expect(user_version(database.path) == 4,
               "construction order ends at schema v4 " + std::to_string(index));
        ++index;
    } while (std::next_permutation(order.begin(), order.end()));
    expect(index == 24, "all four-store construction orders are covered");
}

} // namespace

int main()
{
    test_atomic_acceptance_and_scope_policy();
    test_exact_due_reconciliation_survives_reopen();
    test_revocation_is_permanent_and_due_wins();
    test_clock_rollback_enters_manual_review();
    test_atomic_lifetime_limit_across_connections();
    test_schema_v3_migrates_additively_to_v4();
    test_all_store_construction_orders_accept_v4();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All SQLite wake-intent store tests passed\n";
    return 0;
}
