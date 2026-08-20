#include <gaudere/work/Runtime.hpp>

#include <cstdint>
#include <limits>
#include <utility>

namespace gaudere::work {
namespace {

bool active(const TaskStatus status) noexcept
{
    return status == TaskStatus::running || status == TaskStatus::cancel_requested;
}

TaskResult failure_result(std::string code,
                          std::string message,
                          std::string metadata_content_type = {},
                          std::string metadata = {})
{
    return TaskResult{"text/plain", {}, std::move(code), std::move(message),
                      std::move(metadata_content_type), std::move(metadata)};
}

} // namespace

bool is_terminal(const TaskStatus status) noexcept
{
    return status == TaskStatus::succeeded || status == TaskStatus::failed
        || status == TaskStatus::cancelled || status == TaskStatus::manual_review;
}

bool valid_definition(const Task& task) noexcept
{
    if (task.id.empty() || task.idempotency_key.empty() || task.kind.empty()
        || task.input_content_type.empty()) {
        return false;
    }
    constexpr auto sqlite_integer_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (task.limits.max_input_bytes == 0 || task.limits.max_output_bytes == 0
        || task.limits.max_input_bytes > sqlite_integer_max
        || task.limits.max_output_bytes > sqlite_integer_max
        || task.limits.max_runtime.count() <= 0 || task.limits.max_attempts == 0) {
        return false;
    }
    if (task.input.size() > task.limits.max_input_bytes) {
        return false;
    }
    return task.status == TaskStatus::pending && task.attempts_started == 0
        && !task.lease && task.cancel_reason.empty() && !task.result;
}

Runtime::Runtime(TaskStore& store, Now now)
    : store_(store), now_(std::move(now))
{
}

RuntimeState Runtime::state() const noexcept
{
    return state_;
}

void Runtime::recover()
{
    static_cast<void>(recover_expired());
    state_ = RuntimeState::running;
}

std::size_t Runtime::recover_expired()
{
    std::size_t recovered = 0;
    for (auto task : store_.leased_with_expired_lease(now_())) {
        task.lease.reset();
        if (task.status == TaskStatus::cancel_requested) {
            task.status = TaskStatus::cancelled;
            task.result = failure_result("cancelled", task.cancel_reason);
        } else if (task.attempts_started < task.limits.max_attempts) {
            task.status = TaskStatus::pending;
        } else {
            task.status = TaskStatus::failed;
            task.result = failure_result(
                "attempt_limit_exhausted",
                "task lease expired after the maximum number of attempts");
        }
        store_.save(task);
        ++recovered;
    }
    return recovered;
}

std::optional<TimePoint> Runtime::next_recovery_at() const
{
    return store_.next_lease_expiry();
}

SubmitResult Runtime::submit(const Task& task)
{
    if (state_ != RuntimeState::running) {
        return SubmitResult::unavailable;
    }
    if (!valid_definition(task)) {
        return SubmitResult::invalid;
    }
    if (store_.find(task.id) || store_.find_by_idempotency_key(task.idempotency_key)) {
        return SubmitResult::duplicate;
    }
    store_.save(task);
    return SubmitResult::accepted;
}

bool Runtime::start(const std::string& id, std::string lease_owner)
{
    if (state_ != RuntimeState::running || lease_owner.empty()) {
        return false;
    }
    auto task = store_.find(id);
    if (!task || task->status != TaskStatus::pending
        || task->attempts_started >= task->limits.max_attempts) {
        return false;
    }
    ++task->attempts_started;
    task->status = TaskStatus::running;
    task->lease = Lease{std::move(lease_owner), now_() + task->limits.max_runtime};
    store_.save(*task);
    return true;
}

FinishResult Runtime::succeed(const std::string& id,
                              std::string output,
                              std::string content_type,
                              std::string metadata_content_type,
                              std::string metadata)
{
    auto task = store_.find(id);
    if (!task || !active(task->status) || content_type.empty()
        || (metadata_content_type.empty() != metadata.empty())) {
        return FinishResult::unavailable;
    }
    if (output.size() > task->limits.max_output_bytes) {
        task->status = TaskStatus::failed;
        task->lease.reset();
        task->result = failure_result(
            "output_limit_exceeded",
            "task output exceeded max_output_bytes");
        store_.save(*task);
        return FinishResult::output_limit_exceeded;
    }
    task->status = TaskStatus::succeeded;
    task->lease.reset();
    task->result = TaskResult{std::move(content_type), std::move(output), {}, {},
                              std::move(metadata_content_type), std::move(metadata)};
    store_.save(*task);
    return FinishResult::accepted;
}

bool Runtime::finish_failure(const std::string& id,
                             const TaskStatus status,
                             std::string failure_code,
                             std::string failure_message,
                             std::string metadata_content_type,
                             std::string metadata)
{
    auto task = store_.find(id);
    if (!task || !active(task->status) || failure_code.empty()
        || (metadata_content_type.empty() != metadata.empty())) {
        return false;
    }
    task->status = status;
    task->lease.reset();
    task->result = failure_result(std::move(failure_code), std::move(failure_message),
                                  std::move(metadata_content_type), std::move(metadata));
    store_.save(*task);
    return true;
}

bool Runtime::fail(const std::string& id,
                   std::string failure_code,
                   std::string failure_message,
                   std::string metadata_content_type,
                   std::string metadata)
{
    return finish_failure(id, TaskStatus::failed,
                          std::move(failure_code), std::move(failure_message),
                          std::move(metadata_content_type), std::move(metadata));
}

bool Runtime::require_manual_review(const std::string& id,
                                    std::string failure_code,
                                    std::string failure_message,
                                    std::string metadata_content_type,
                                    std::string metadata)
{
    return finish_failure(id, TaskStatus::manual_review,
                          std::move(failure_code), std::move(failure_message),
                          std::move(metadata_content_type), std::move(metadata));
}

bool Runtime::request_cancel(const std::string& id, std::string reason)
{
    auto task = store_.find(id);
    if (!task || is_terminal(task->status)) {
        return false;
    }
    if (task->status == TaskStatus::pending) {
        task->status = TaskStatus::cancelled;
        task->cancel_reason = std::move(reason);
        task->result = failure_result("cancelled", task->cancel_reason);
    } else if (task->status == TaskStatus::running) {
        task->status = TaskStatus::cancel_requested;
        task->cancel_reason = std::move(reason);
    } else if (task->status != TaskStatus::cancel_requested) {
        return false;
    }
    store_.save(*task);
    return true;
}

bool Runtime::mark_cancelled(const std::string& id)
{
    auto task = store_.find(id);
    if (!task || task->status != TaskStatus::cancel_requested) {
        return false;
    }
    task->status = TaskStatus::cancelled;
    task->lease.reset();
    task->result = failure_result("cancelled", task->cancel_reason);
    store_.save(*task);
    return true;
}

void Runtime::request_shutdown() noexcept
{
    if (state_ == RuntimeState::running) {
        state_ = RuntimeState::draining;
    }
}

bool Runtime::try_mark_safe()
{
    if (state_ != RuntimeState::draining || store_.has_active()) {
        return false;
    }
    state_ = RuntimeState::safe;
    return true;
}

} // namespace gaudere::work
