#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace gaudere::scheduling::wake;
using namespace std::chrono_literals;

class MemoryWakeIntentStore final : public WakeIntentStore {
public:
    std::optional<WakeIntent> find(const std::string& scope,
                                   const std::string& id) const override
    {
        const auto found = intents.find({scope, id});
        return found == intents.end() ? std::nullopt
                                      : std::optional<WakeIntent>{found->second};
    }

    std::optional<WakeIntent> find_by_source(
        const std::string& scope,
        const std::string& source_id) const override
    {
        for (const auto& entry : intents) {
            if (entry.second.scope == scope
                && entry.second.source_id == source_id) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

    WakeIntentAcceptResult accept(const WakeIntent& intent,
                                  const WakeIntentPolicy& policy) override
    {
        if (!valid_new_wake_intent(intent) || !valid_wake_intent_policy(policy)) {
            return WakeIntentAcceptResult::invalid;
        }
        if (find_by_source(intent.scope, intent.source_id)) {
            return WakeIntentAcceptResult::duplicate;
        }
        if (find(intent.scope, intent.id)) {
            return WakeIntentAcceptResult::conflict;
        }
        const auto used = static_cast<std::uint64_t>(std::count_if(
            intents.begin(), intents.end(), [&](const auto& entry) {
                return entry.second.scope == intent.scope;
            }));
        if (used >= policy.max_total) {
            return WakeIntentAcceptResult::total_exhausted;
        }
        intents[{intent.scope, intent.id}] = intent;
        return WakeIntentAcceptResult::accepted;
    }

    std::optional<WakeIntentTimePoint> next_scheduled_at(
        const std::string& scope) const override
    {
        std::optional<WakeIntentTimePoint> result;
        for (const auto& entry : intents) {
            const auto& intent = entry.second;
            if (intent.scope == scope && intent.status == WakeIntentStatus::scheduled
                && (!result || intent.due_at < *result)) {
                result = intent.due_at;
            }
        }
        return result;
    }

    WakeIntentReconcileResult reconcile(const std::string& scope,
                                         const WakeIntentTimePoint now) override
    {
        WakeIntentReconcileResult result;
        for (auto& entry : intents) {
            auto& intent = entry.second;
            if (intent.scope != scope || intent.status != WakeIntentStatus::scheduled) {
                continue;
            }
            if (now < intent.accepted_at) {
                intent.status = WakeIntentStatus::manual_review;
                intent.terminal_at = now;
                intent.terminal_reason = "clock_rollback";
                ++result.manual_review;
            } else if (now >= intent.due_at) {
                intent.status = WakeIntentStatus::fired;
                intent.terminal_at = now;
                ++result.fired;
            }
        }
        return result;
    }

    WakeIntentRevokeResult revoke(const std::string& scope,
                                  const std::string& id,
                                  const WakeIntentTimePoint now,
                                  const std::string& reason) override
    {
        auto found = intents.find({scope, id});
        if (found == intents.end()) {
            return WakeIntentRevokeResult::not_found;
        }
        auto& intent = found->second;
        if (is_terminal(intent.status)) {
            return WakeIntentRevokeResult::terminal;
        }
        intent.terminal_at = now;
        if (now < intent.accepted_at) {
            intent.status = WakeIntentStatus::manual_review;
            intent.terminal_reason = "clock_rollback";
            return WakeIntentRevokeResult::manual_review;
        }
        if (now >= intent.due_at) {
            intent.status = WakeIntentStatus::fired;
            return WakeIntentRevokeResult::fired;
        }
        intent.status = WakeIntentStatus::revoked;
        intent.terminal_reason = reason;
        return WakeIntentRevokeResult::revoked;
    }

    std::map<std::pair<std::string, std::string>, WakeIntent> intents;
};

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_configuration_validation()
{
    MemoryWakeIntentStore store;
    WakeIntentPolicy policy{1};

    bool empty_clock = false;
    try {
        static_cast<void>(WakeIntentRuntime{
            store, WakeIntentRuntime::Now{}, "scope", policy});
    } catch (const std::invalid_argument&) {
        empty_clock = true;
    }
    expect(empty_clock, "empty clock is rejected");

    bool empty_scope = false;
    try {
        static_cast<void>(WakeIntentRuntime{
            store, [] { return WakeIntentTimePoint{}; }, {}, policy});
    } catch (const std::invalid_argument&) {
        empty_scope = true;
    }
    expect(empty_scope, "empty scope is rejected");

    bool invalid_policy = false;
    try {
        static_cast<void>(WakeIntentRuntime{
            store, [] { return WakeIntentTimePoint{}; }, "scope", {}});
    } catch (const std::invalid_argument&) {
        invalid_policy = true;
    }
    expect(invalid_policy, "zero lifetime policy is rejected");
}

void test_acceptance_is_bounded_and_scoped()
{
    MemoryWakeIntentStore store;
    auto now = WakeIntentTimePoint{1000ms + 999us};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "cognition.wake.v0", {1});

    expect(runtime.accept("wake-a", "source-a", 900s)
               == WakeIntentAcceptResult::accepted,
           "first scoped wake is accepted");
    const auto accepted = runtime.find("wake-a");
    expect(accepted && accepted->scope == "cognition.wake.v0"
               && accepted->accepted_at == WakeIntentTimePoint{1000ms}
               && accepted->due_at == WakeIntentTimePoint{901000ms}
               && accepted->status == WakeIntentStatus::scheduled,
           "runtime fixes scope and persists one millisecond deadline");

    now += 10s;
    expect(runtime.accept("different-id", "source-a", 1s)
               == WakeIntentAcceptResult::duplicate,
           "source identity is idempotent without replacing the deadline");
    expect(runtime.find("wake-a")->due_at == WakeIntentTimePoint{901000ms},
           "duplicate acceptance preserves the first deadline");
    expect(runtime.accept("wake-b", "source-b", 1s)
               == WakeIntentAcceptResult::total_exhausted,
           "per-scope lifetime bound rejects a second source");

    WakeIntentRuntime other(store, [&now] { return now; }, "other.scope", {1});
    expect(other.accept("wake-b", "source-b", 1s)
               == WakeIntentAcceptResult::accepted,
           "independent core scopes have independent policies");
    expect(runtime.scope() == "cognition.wake.v0" && runtime.policy().max_total == 1,
           "fixed runtime capability is observable");
}

void test_invalid_acceptance_never_reaches_store()
{
    MemoryWakeIntentStore store;
    auto now = WakeIntentTimePoint{};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "scope", {1});

    expect(runtime.accept({}, "source", 1s) == WakeIntentAcceptResult::invalid,
           "empty wake identity is rejected");
    expect(runtime.accept("wake", {}, 1s) == WakeIntentAcceptResult::invalid,
           "empty source identity is rejected");
    expect(runtime.accept("wake", "source", 0ms) == WakeIntentAcceptResult::invalid,
           "zero delay is rejected");
    expect(runtime.accept(std::string(wake_intent_identifier_max_bytes + 1, 'x'),
                          "source", 1s) == WakeIntentAcceptResult::invalid,
           "oversized wake identity is rejected");

    now = WakeIntentTimePoint::max() - 500ms;
    expect(runtime.accept("wake", "source", 1s) == WakeIntentAcceptResult::invalid,
           "deadline overflow fails before persistence");
    expect(store.intents.empty(), "invalid acceptance writes no durable intent");
}

void test_pre_epoch_deadline_uses_checked_addition()
{
    MemoryWakeIntentStore store;
    auto now = WakeIntentTimePoint{-500us};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "scope", {1});

    expect(runtime.accept("wake", "source", 1s)
               == WakeIntentAcceptResult::accepted,
           "pre-epoch clock sample does not overflow deadline arithmetic");
    const auto accepted = runtime.find("wake");
    expect(accepted && accepted->accepted_at == WakeIntentTimePoint{-1ms}
               && accepted->due_at == WakeIntentTimePoint{999ms},
           "negative clock sample is floored and advanced exactly");
}

void test_exact_reconciliation_and_clock_rollback()
{
    MemoryWakeIntentStore store;
    auto now = WakeIntentTimePoint{100s};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "scope", {3});
    expect(runtime.accept("early", "source-early", 10s)
               == WakeIntentAcceptResult::accepted,
           "early wake accepted");
    expect(runtime.accept("later", "source-later", 20s)
               == WakeIntentAcceptResult::accepted,
           "later wake accepted");
    expect(runtime.next_scheduled_at() == WakeIntentTimePoint{110s},
           "earliest exact durable deadline is exposed");

    now = WakeIntentTimePoint{110s};
    const auto due = runtime.reconcile();
    expect(due.fired == 1 && due.manual_review == 0,
           "deadline fires exactly at its inclusive boundary");
    expect(runtime.find("early")->status == WakeIntentStatus::fired
               && runtime.find("early")->terminal_at == now,
           "firing persists its observation time");
    expect(runtime.next_scheduled_at() == WakeIntentTimePoint{120s},
           "terminal wake is removed from future scheduling");
    expect(runtime.reconcile().fired == 0,
           "reconciliation never repeats a terminal wake");

    now = WakeIntentTimePoint{90s};
    const auto rollback = runtime.reconcile();
    expect(rollback.manual_review == 1 && rollback.fired == 0,
           "clock rollback fails closed");
    expect(runtime.find("later")->status == WakeIntentStatus::manual_review
               && runtime.find("later")->terminal_reason == "clock_rollback",
           "detected rollback is durable manual review");
    expect(!runtime.next_scheduled_at(),
           "manual-review wake is never automatically re-armed");
}

void test_revocation_and_due_ordering()
{
    MemoryWakeIntentStore store;
    auto now = WakeIntentTimePoint{200s};
    WakeIntentRuntime runtime(store, [&now] { return now; }, "scope", {3});

    expect(runtime.accept("revoke", "source-revoke", 10s)
               == WakeIntentAcceptResult::accepted,
           "revocation fixture accepted");
    now = WakeIntentTimePoint{209s};
    expect(runtime.revoke("revoke", "operator request")
               == WakeIntentRevokeResult::revoked,
           "operator can revoke strictly before due");
    expect(runtime.find("revoke")->status == WakeIntentStatus::revoked
               && runtime.find("revoke")->terminal_reason == "operator request",
           "revocation and reason are durable");
    expect(runtime.revoke("revoke", "again") == WakeIntentRevokeResult::terminal,
           "terminal wake cannot be changed");

    now = WakeIntentTimePoint{200s};
    expect(runtime.accept("due", "source-due", 10s)
               == WakeIntentAcceptResult::accepted,
           "due-order fixture accepted");
    now = WakeIntentTimePoint{210s};
    expect(runtime.revoke("due", "too late") == WakeIntentRevokeResult::fired,
           "firing wins at the exact deadline");
    expect(runtime.find("due")->status == WakeIntentStatus::fired,
           "late revoke persists fired rather than revoked");

    expect(runtime.revoke("missing", "reason") == WakeIntentRevokeResult::not_found,
           "unknown wake is reported");
    expect(runtime.revoke("due", {}) == WakeIntentRevokeResult::invalid,
           "empty revocation reason is rejected");
}

} // namespace

int main()
{
    test_configuration_validation();
    test_acceptance_is_bounded_and_scoped();
    test_invalid_acceptance_never_reaches_store();
    test_pre_epoch_deadline_uses_checked_addition();
    test_exact_reconciliation_and_clock_rollback();
    test_revocation_and_due_ordering();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All wake-intent runtime tests passed\n";
    return 0;
}
