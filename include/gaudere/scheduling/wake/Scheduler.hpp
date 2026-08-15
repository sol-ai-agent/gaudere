#ifndef GAUDERE_SCHEDULING_WAKE_SCHEDULER_HPP
#define GAUDERE_SCHEDULING_WAKE_SCHEDULER_HPP

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace gaudere::scheduling::wake {

enum class Update {
    scheduled,
    advanced,
    unchanged
};

enum class WaitResult {
    due,
    stopped
};

/**
 * Thread-safe owner of one wake-up deadline.
 *
 * The scheduler owns no thread. wait() blocks the calling thread and consumes
 * a due deadline. stop() is permanent and discards any pending deadline.
 */
class Scheduler {
public:
    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    Update request_after(Duration delay);
    Update request_at(TimePoint deadline);

    [[nodiscard]] std::optional<TimePoint> next() const;

    WaitResult wait();
    void stop();

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<TimePoint> deadline_;
    bool stopped_ = false;
};

} // namespace gaudere::scheduling::wake

#endif
