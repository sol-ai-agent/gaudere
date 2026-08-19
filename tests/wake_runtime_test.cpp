#include <gaudere/scheduling/wake/Runtime.hpp>

#include <chrono>
#include <iostream>
#include <map>
#include <string>

namespace {

using namespace gaudere::scheduling::wake;
using namespace std::chrono_literals;

class MemoryActionStore final : public ActionStore {
public:
    std::optional<Action> find(const std::string& id) const override
    {
        const auto found = actions.find(id);
        return found == actions.end() ? std::nullopt
                                      : std::optional<Action>{found->second};
    }

    std::optional<Action> find_by_idempotency_key(const std::string& key) const override
    {
        for (const auto& entry : actions) {
            if (entry.second.idempotency_key == key) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

    std::vector<Action> running_with_expired_lease(const TimePoint now) const override
    {
        std::vector<Action> result;
        for (const auto& entry : actions) {
            const auto& action = entry.second;
            if (action.status == ActionStatus::running && action.lease
                && action.lease->expires_at <= now) {
                result.push_back(action);
            }
        }
        return result;
    }

    bool has_running() const override
    {
        for (const auto& entry : actions) {
            if (entry.second.status == ActionStatus::running) {
                return true;
            }
        }
        return false;
    }

    void save(const Action& action) override { actions[action.id] = action; }

    std::map<std::string, Action> actions;
};

int failures = 0;
void expect(const bool value, const std::string& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Action action(std::string id, std::string key, const bool critical = false)
{
    return Action{std::move(id), std::move(key), critical,
                  ActionStatus::pending, EffectResult::none, std::nullopt};
}

void test_transitions_and_idempotence()
{
    MemoryActionStore store;
    const TimePoint now{};
    Runtime runtime(store, [now] { return now; });
    runtime.recover();

    expect(runtime.submit(action("a", "key")) == SubmitResult::accepted,
           "first action is accepted");
    expect(runtime.submit(action("b", "key")) == SubmitResult::duplicate,
           "idempotency key is logically unique");
    expect(runtime.start("a", "worker", 10s), "pending action starts");
    expect(runtime.transition("a", ActionStatus::succeeded), "effect-free running action succeeds");
    expect(!runtime.transition("a", ActionStatus::running), "terminal action cannot restart");
}

void test_draining()
{
    MemoryActionStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();
    runtime.request_shutdown();

    expect(runtime.state() == RuntimeState::draining, "shutdown enters draining");
    expect(runtime.submit(action("critical", "critical", true))
               == SubmitResult::critical_rejected,
           "draining rejects a critical action");
    expect(runtime.submit(action("ordinary", "ordinary")) == SubmitResult::accepted,
           "draining still accepts a non-critical action");
    expect(runtime.try_mark_safe(), "draining with no running action becomes safe");
}

void test_recovery()
{
    MemoryActionStore store;
    const TimePoint now = TimePoint{} + 10s;
    auto retry = action("retry", "retry");
    retry.status = ActionStatus::running;
    retry.lease = Lease{"worker", now - 1s};
    store.save(retry);

    auto unknown = action("unknown", "unknown");
    unknown.status = ActionStatus::running;
    unknown.effect_result = EffectResult::unknown;
    unknown.lease = Lease{"worker", now - 1s};
    store.save(unknown);

    auto confirmed = action("confirmed", "confirmed");
    confirmed.status = ActionStatus::running;
    confirmed.effect_result = EffectResult::confirmed;
    confirmed.lease = Lease{"worker", now - 1s};
    store.save(confirmed);

    Runtime runtime(store, [now] { return now; });
    expect(runtime.state() == RuntimeState::recovering, "runtime starts recovering");
    runtime.recover();
    expect(store.find("retry")->status == ActionStatus::retry_wait,
           "expired lease with no external effect becomes retry_wait");
    expect(store.find("unknown")->status == ActionStatus::manual_review,
           "unknown expired effect requires manual review");
    expect(store.find("confirmed")->status == ActionStatus::succeeded,
           "confirmed expired effect recovers as succeeded instead of retrying");
}

void test_external_effect_boundary()
{
    MemoryActionStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(action("confirmed", "confirmed")) == SubmitResult::accepted,
           "external-effect action is accepted");
    expect(runtime.start("confirmed", "worker", 5s),
           "external-effect action starts with a lease");
    expect(runtime.record_effect_started("confirmed"),
           "external-effect start is persisted before the boundary is crossed");

    const auto started = store.find("confirmed");
    expect(started && started->status == ActionStatus::running
               && started->effect_result == EffectResult::unknown
               && started->lease,
           "effect-start marker keeps the action running but makes retry unsafe");
    expect(!runtime.transition("confirmed", ActionStatus::retry_wait),
           "unknown external effect cannot be blindly retried");
    expect(!runtime.transition("confirmed", ActionStatus::succeeded),
           "unknown external effect cannot bypass explicit confirmation");
    expect(runtime.record_confirmed_result("confirmed"),
           "definite provider result confirms the external effect");

    const auto done = store.find("confirmed");
    expect(done && done->status == ActionStatus::succeeded
               && done->effect_result == EffectResult::confirmed
               && !done->lease,
           "confirmed external effect becomes terminal and releases its lease");
}

void test_crash_after_effect_start_requires_manual_review()
{
    MemoryActionStore store;
    const TimePoint started_at{};
    Runtime first(store, [started_at] { return started_at; });
    first.recover();
    expect(first.submit(action("crash", "crash")) == SubmitResult::accepted,
           "crash action is accepted");
    expect(first.start("crash", "worker", 1s), "crash action starts");
    expect(first.record_effect_started("crash"),
           "possible external effect is durable before simulated process death");

    const TimePoint recovered_at = started_at + 2s;
    Runtime replacement(store, [recovered_at] { return recovered_at; });
    replacement.recover();
    const auto recovered = store.find("crash");
    expect(recovered && recovered->status == ActionStatus::manual_review
               && recovered->effect_result == EffectResult::unknown
               && !recovered->lease,
           "expired action after effect start never retries automatically");
}

void test_unknown_result()
{
    MemoryActionStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(action("a", "a")) == SubmitResult::accepted,
           "action for unknown result is accepted");
    expect(runtime.start("a", "worker", 1s),
           "action for unknown result starts");
    expect(runtime.record_unknown_result("a"), "unknown result is recorded");
    expect(store.find("a")->status == ActionStatus::manual_review,
           "unknown result enters manual review");
}

} // namespace

int main()
{
    test_transitions_and_idempotence();
    test_draining();
    test_recovery();
    test_external_effect_boundary();
    test_crash_after_effect_start_requires_manual_review();
    test_unknown_result();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All wake runtime tests passed\n";
    return 0;
}
