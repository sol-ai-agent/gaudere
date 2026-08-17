#include <gaudere/work/Runtime.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <utility>

namespace {

using namespace gaudere::work;
using namespace std::chrono_literals;

class MemoryTaskStore final : public TaskStore {
public:
    std::optional<Task> find(const std::string& id) const override
    {
        const auto found = tasks.find(id);
        return found == tasks.end() ? std::nullopt
                                    : std::optional<Task>{found->second};
    }

    std::optional<Task> find_by_idempotency_key(const std::string& key) const override
    {
        for (const auto& entry : tasks) {
            if (entry.second.idempotency_key == key) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

    std::optional<Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const override
    {
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if (task.status == TaskStatus::pending
                && std::find(accepted_kinds.begin(), accepted_kinds.end(), task.kind)
                    != accepted_kinds.end()) {
                return task;
            }
        }
        return std::nullopt;
    }

    std::vector<Task> leased_with_expired_lease(const TimePoint now) const override
    {
        std::vector<Task> result;
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if ((task.status == TaskStatus::running
                 || task.status == TaskStatus::cancel_requested)
                && task.lease && task.lease->expires_at <= now) {
                result.push_back(task);
            }
        }
        return result;
    }

    std::optional<TimePoint> next_lease_expiry() const override
    {
        std::optional<TimePoint> result;
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if ((task.status == TaskStatus::running
                 || task.status == TaskStatus::cancel_requested)
                && task.lease && (!result || task.lease->expires_at < *result)) {
                result = task.lease->expires_at;
            }
        }
        return result;
    }

    bool has_active() const override
    {
        for (const auto& entry : tasks) {
            if (entry.second.status == TaskStatus::running
                || entry.second.status == TaskStatus::cancel_requested) {
                return true;
            }
        }
        return false;
    }

    void save(const Task& task) override { tasks[task.id] = task; }

    std::map<std::string, Task> tasks;
};

int failures = 0;

void expect(const bool value, const std::string& message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Task task(std::string id, std::string key, std::string input = "hello")
{
    Task value;
    value.id = std::move(id);
    value.idempotency_key = std::move(key);
    value.kind = "test.echo";
    value.input_content_type = "text/plain";
    value.input = std::move(input);
    value.limits.max_input_bytes = 16;
    value.limits.max_output_bytes = 16;
    value.limits.max_runtime = 10s;
    value.limits.max_attempts = 2;
    return value;
}

void test_submit_and_success()
{
    MemoryTaskStore store;
    const TimePoint now{};
    Runtime runtime(store, [now] { return now; });
    runtime.recover();

    expect(runtime.submit(task("a", "key")) == SubmitResult::accepted,
           "bounded task is accepted");
    expect(runtime.submit(task("b", "key")) == SubmitResult::duplicate,
           "idempotency key rejects duplicate work");
    expect(runtime.start("a", "worker"), "pending task starts");

    const auto running = store.find("a");
    expect(running && running->attempts_started == 1 && running->lease
               && running->lease->expires_at == now + 10s,
           "start creates one bounded runtime lease");

    expect(runtime.succeed("a", "world", "text/plain") == FinishResult::accepted,
           "running task succeeds");
    const auto done = store.find("a");
    expect(done && done->status == TaskStatus::succeeded && done->result
               && done->result->output == "world" && !done->lease,
           "success persists output and clears lease");
}

void test_definition_and_output_limits()
{
    MemoryTaskStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    auto too_large = task("large-input", "large-input", "0123456789abcdefx");
    expect(runtime.submit(too_large) == SubmitResult::invalid,
           "input beyond max_input_bytes is rejected before persistence");

    auto limited = task("limited", "limited");
    limited.limits.max_output_bytes = 4;
    expect(runtime.submit(limited) == SubmitResult::accepted,
           "output-limited task is accepted");
    expect(runtime.start("limited", "worker"), "output-limited task starts");
    expect(runtime.succeed("limited", "12345", "text/plain")
               == FinishResult::output_limit_exceeded,
           "oversized output is not persisted as success");
    const auto failed = store.find("limited");
    expect(failed && failed->status == TaskStatus::failed && failed->result
               && failed->result->failure_code == "output_limit_exceeded",
           "oversized output becomes a durable bounded failure");
}

void test_kind_filtered_selection()
{
    MemoryTaskStore store;
    auto unsupported = task("a-unsupported", "a-unsupported");
    unsupported.kind = "provider.missing";
    store.save(unsupported);
    auto supported = task("b-supported", "b-supported");
    supported.kind = "local.echo";
    store.save(supported);

    expect(!store.find_pending_for({}),
           "empty accepted kind set selects no pending work");
    const auto selected = store.find_pending_for({"local.echo"});
    expect(selected && selected->id == "b-supported",
           "selection skips pending tasks whose kind has no registered handler");

    supported.status = TaskStatus::running;
    supported.attempts_started = 1;
    supported.lease = Lease{"worker", TimePoint{} + 1s};
    store.save(supported);
    expect(!store.find_pending_for({"local.echo"}),
           "selection never returns active work");
}

void test_cancellation()
{
    MemoryTaskStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    expect(runtime.submit(task("pending", "pending")) == SubmitResult::accepted,
           "pending cancellation task is accepted");
    expect(runtime.request_cancel("pending", "operator request"),
           "pending task cancels immediately");
    expect(store.find("pending")->status == TaskStatus::cancelled,
           "pending cancellation is terminal");

    expect(runtime.submit(task("running", "running")) == SubmitResult::accepted,
           "running cancellation task is accepted");
    expect(runtime.start("running", "worker"), "cancellation task starts");
    expect(runtime.request_cancel("running", "operator request"),
           "running task records cancellation request");
    expect(store.find("running")->status == TaskStatus::cancel_requested,
           "running cancellation remains cooperative");
    expect(runtime.mark_cancelled("running"), "worker acknowledges cancellation");
    expect(store.find("running")->status == TaskStatus::cancelled,
           "acknowledged cancellation is terminal");
}

void test_recovery_and_attempt_limit()
{
    MemoryTaskStore store;
    const TimePoint now = TimePoint{} + 20s;

    auto retry = task("retry", "retry");
    retry.status = TaskStatus::running;
    retry.attempts_started = 1;
    retry.lease = Lease{"worker", now - 1s};
    store.save(retry);

    auto exhausted = task("exhausted", "exhausted");
    exhausted.status = TaskStatus::running;
    exhausted.attempts_started = 2;
    exhausted.lease = Lease{"worker", now - 1s};
    store.save(exhausted);

    auto cancelling = task("cancelling", "cancelling");
    cancelling.status = TaskStatus::cancel_requested;
    cancelling.attempts_started = 1;
    cancelling.cancel_reason = "shutdown";
    cancelling.lease = Lease{"worker", now - 1s};
    store.save(cancelling);

    Runtime runtime(store, [now] { return now; });
    runtime.recover();
    expect(store.find("retry")->status == TaskStatus::pending,
           "expired task lease retries within attempt budget");
    expect(store.find("exhausted")->status == TaskStatus::failed
               && store.find("exhausted")->result->failure_code
                    == "attempt_limit_exhausted",
           "expired task lease fails at attempt limit");
    expect(store.find("cancelling")->status == TaskStatus::cancelled,
           "expired cancellation completes during recovery");
}

void test_unexpired_lease_has_future_recovery_deadline()
{
    MemoryTaskStore store;
    TimePoint now = TimePoint{} + 10s;

    auto interrupted = task("interrupted", "interrupted");
    interrupted.status = TaskStatus::running;
    interrupted.attempts_started = 1;
    interrupted.lease = Lease{"dead-worker", now + 5s};
    store.save(interrupted);

    Runtime runtime(store, [&now] { return now; });
    runtime.recover();
    expect(store.find("interrupted")->status == TaskStatus::running,
           "startup does not steal an unexpired lease");
    expect(runtime.next_recovery_at() == now + 5s,
           "runtime exposes the future lease deadline that must wake recovery");

    now += 6s;
    expect(runtime.recover_expired() == 1,
           "expired lease can be recovered after startup without restarting");
    expect(store.find("interrupted")->status == TaskStatus::pending,
           "post-startup recovery returns interrupted work to pending");
    expect(!runtime.next_recovery_at(),
           "terminal or pending work no longer schedules lease recovery");
}

void test_manual_review_and_draining()
{
    MemoryTaskStore store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(task("review", "review")) == SubmitResult::accepted,
           "review task is accepted");
    expect(runtime.start("review", "worker"), "review task starts");
    expect(runtime.require_manual_review(
               "review", "external_effect_unknown", "reconcile before retry"),
           "uncertain effect enters manual review");
    expect(store.find("review")->status == TaskStatus::manual_review,
           "manual review is terminal for automatic execution");

    runtime.request_shutdown();
    expect(runtime.submit(task("late", "late")) == SubmitResult::unavailable,
           "draining rejects new work");
    expect(runtime.try_mark_safe(), "draining without active task becomes safe");
}

} // namespace

int main()
{
    test_submit_and_success();
    test_definition_and_output_limits();
    test_kind_filtered_selection();
    test_cancellation();
    test_recovery_and_attempt_limit();
    test_unexpired_lease_has_future_recovery_deadline();
    test_manual_review_and_draining();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All work runtime tests passed\n";
    return 0;
}
