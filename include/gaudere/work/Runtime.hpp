#ifndef GAUDERE_WORK_RUNTIME_HPP
#define GAUDERE_WORK_RUNTIME_HPP

#include <gaudere/work/TaskStore.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace gaudere::work {

enum class RuntimeState {
    recovering,
    running,
    draining,
    safe
};

enum class SubmitResult {
    accepted,
    duplicate,
    invalid,
    unavailable
};

enum class FinishResult {
    accepted,
    output_limit_exceeded,
    unavailable
};

class Runtime {
public:
    using Now = std::function<TimePoint()>;

    explicit Runtime(TaskStore& store, Now now);

    [[nodiscard]] RuntimeState state() const noexcept;
    void recover();
    [[nodiscard]] std::size_t recover_expired();
    [[nodiscard]] std::optional<TimePoint> next_recovery_at() const;
    [[nodiscard]] SubmitResult submit(const Task& task);
    [[nodiscard]] bool start(const std::string& id, std::string lease_owner);
    [[nodiscard]] FinishResult succeed(const std::string& id,
                                       std::string output,
                                       std::string content_type);
    [[nodiscard]] bool fail(const std::string& id,
                            std::string failure_code,
                            std::string failure_message);
    [[nodiscard]] bool require_manual_review(const std::string& id,
                                             std::string failure_code,
                                             std::string failure_message);
    [[nodiscard]] bool request_cancel(const std::string& id, std::string reason);
    [[nodiscard]] bool mark_cancelled(const std::string& id);
    void request_shutdown() noexcept;
    [[nodiscard]] bool try_mark_safe();

private:
    [[nodiscard]] bool finish_failure(const std::string& id,
                                      TaskStatus status,
                                      std::string failure_code,
                                      std::string failure_message);

    TaskStore& store_;
    Now now_;
    RuntimeState state_ = RuntimeState::recovering;
};

} // namespace gaudere::work

#endif
