#ifndef GAUDERE_WORK_TASK_HPP
#define GAUDERE_WORK_TASK_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere::work {

using TimePoint = std::chrono::system_clock::time_point;

struct ResourceLimits {
    std::uint64_t max_input_bytes = 64 * 1024;
    std::uint64_t max_output_bytes = 64 * 1024;
    std::chrono::milliseconds max_runtime{5 * 60 * 1000};
    std::uint32_t max_attempts = 1;
};

enum class TaskStatus {
    pending,
    running,
    cancel_requested,
    succeeded,
    failed,
    cancelled,
    manual_review
};

struct Lease {
    std::string owner;
    TimePoint expires_at;
};

struct TaskResult {
    std::string content_type;
    std::string output;
    std::string failure_code;
    std::string failure_message;
};

struct Task {
    std::string id;
    std::string idempotency_key;
    std::string kind;
    std::string input_content_type;
    std::string input;
    ResourceLimits limits;
    std::uint32_t attempts_started = 0;
    TaskStatus status = TaskStatus::pending;
    std::optional<Lease> lease;
    std::string cancel_reason;
    std::optional<TaskResult> result;
};

[[nodiscard]] bool is_terminal(TaskStatus status) noexcept;
[[nodiscard]] bool valid_definition(const Task& task) noexcept;

} // namespace gaudere::work

#endif
