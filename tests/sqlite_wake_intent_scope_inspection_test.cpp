#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using SqliteWakeIntentStore = gaudere::persistence::sqlite::WakeIntentStore;
using namespace gaudere::scheduling::wake;
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
            / ("gaudere-wake-scope-test-" + std::to_string(
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

void test_empty_one_ambiguous_and_scope_isolation()
{
    TemporaryDatabase database;
    SqliteWakeIntentStore store(database.path.string());

    const auto empty = store.inspect_scope("scope-a");
    expect(empty.result == WakeIntentScopeResult::empty && !empty.intent,
           "empty scope reports no record");

    auto now = WakeIntentTimePoint{100s};
    WakeIntentRuntime a(store, [&now] { return now; }, "scope-a", {3});
    expect(a.accept("wake-a", "source-a", 10s) == WakeIntentAcceptResult::accepted,
           "first wake accepted");

    const auto one = a.inspect_scope();
    expect(one.result == WakeIntentScopeResult::one && one.intent
               && one.intent->scope == "scope-a"
               && one.intent->id == "wake-a"
               && one.intent->source_id == "source-a"
               && one.intent->accepted_at == WakeIntentTimePoint{100s}
               && one.intent->due_at == WakeIntentTimePoint{110s}
               && one.intent->status == WakeIntentStatus::scheduled,
           "one record round-trips through canonical read validation");

    WakeIntentRuntime b(store, [&now] { return now; }, "scope-b", {1});
    expect(b.accept("wake-b", "source-b", 20s) == WakeIntentAcceptResult::accepted,
           "other scope wake accepted");
    const auto isolated = b.inspect_scope();
    expect(isolated.result == WakeIntentScopeResult::one && isolated.intent
               && isolated.intent->id == "wake-b",
           "inspection is fixed to one scope");

    expect(a.accept("wake-a2", "source-a2", 30s) == WakeIntentAcceptResult::accepted,
           "second record in same generic scope accepted for ambiguity test");
    const auto ambiguous = a.inspect_scope();
    expect(ambiguous.result == WakeIntentScopeResult::ambiguous && !ambiguous.intent,
           "two records report ambiguity without arbitrary selection");
}

void test_terminal_record_is_still_discoverable_and_read_only()
{
    TemporaryDatabase database;
    SqliteWakeIntentStore store(database.path.string());
    auto now = WakeIntentTimePoint{200s};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "scope", {1});

    expect(runtime.accept("wake", "source", 10s) == WakeIntentAcceptResult::accepted,
           "terminal fixture accepted");
    now = WakeIntentTimePoint{210s};
    const auto reconciled = runtime.reconcile();
    expect(reconciled.fired == 1, "fixture fires at deadline");

    const auto before = runtime.find("wake");
    const auto inspected = runtime.inspect_scope();
    const auto after = runtime.find("wake");
    expect(inspected.result == WakeIntentScopeResult::one && inspected.intent
               && inspected.intent->status == WakeIntentStatus::fired
               && inspected.intent->terminal_at == WakeIntentTimePoint{210s},
           "terminal row remains discoverable");
    expect(before && after && before->status == after->status
               && before->terminal_at == after->terminal_at
               && before->terminal_reason == after->terminal_reason,
           "scope inspection performs no terminal-state mutation");
}

void test_invalid_scope_and_default_fail_closed()
{
    TemporaryDatabase database;
    SqliteWakeIntentStore store(database.path.string());

    bool invalid_scope = false;
    try {
        static_cast<void>(store.inspect_scope({}));
    } catch (const std::invalid_argument&) {
        invalid_scope = true;
    }
    expect(invalid_scope, "invalid scope is rejected");

    class UnsupportedStore final : public gaudere::scheduling::wake::WakeIntentStore {
    public:
        std::optional<WakeIntent> find(const std::string&, const std::string&) const override
        {
            return std::nullopt;
        }
        std::optional<WakeIntent> find_by_source(
            const std::string&, const std::string&) const override
        {
            return std::nullopt;
        }
        WakeIntentAcceptResult accept(const WakeIntent&, const WakeIntentPolicy&) override
        {
            return WakeIntentAcceptResult::invalid;
        }
        std::optional<WakeIntentTimePoint> next_scheduled_at(
            const std::string&) const override
        {
            return std::nullopt;
        }
        WakeIntentReconcileResult reconcile(
            const std::string&, WakeIntentTimePoint) override
        {
            return {};
        }
        WakeIntentRevokeResult revoke(
            const std::string&, const std::string&, WakeIntentTimePoint,
            const std::string&) override
        {
            return WakeIntentRevokeResult::invalid;
        }
    } unsupported;

    bool unsupported_closed = false;
    try {
        static_cast<void>(unsupported.inspect_scope("scope"));
    } catch (const std::logic_error&) {
        unsupported_closed = true;
    }
    expect(unsupported_closed, "unimplemented generic inspection fails closed");
}

} // namespace

int main()
{
    test_empty_one_ambiguous_and_scope_isolation();
    test_terminal_record_is_still_discoverable_and_read_only();
    test_invalid_scope_and_default_fail_closed();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All SQLite wake scope inspection tests passed\n";
    return 0;
}
