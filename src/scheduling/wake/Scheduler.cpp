#include <gaudere/scheduling/wake/Scheduler.hpp>

namespace gaudere::scheduling::wake {

Update Scheduler::request_after(const Duration delay)
{
    return request_at(Clock::now() + delay);
}

Update Scheduler::request_at(const TimePoint deadline)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (stopped_) {
        return Update::unchanged;
    }

    if (!deadline_) {
        deadline_ = deadline;
        condition_.notify_one();
        return Update::scheduled;
    }

    if (deadline < *deadline_) {
        deadline_ = deadline;
        condition_.notify_one();
        return Update::advanced;
    }

    return Update::unchanged;
}

std::optional<Scheduler::TimePoint> Scheduler::next() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return deadline_;
}

WaitResult Scheduler::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;) {
        if (stopped_) {
            return WaitResult::stopped;
        }

        if (deadline_ && Clock::now() >= *deadline_) {
            deadline_.reset();
            interrupted_ = false;
            return WaitResult::due;
        }

        if (interrupted_) {
            interrupted_ = false;
            return WaitResult::interrupted;
        }

        if (!deadline_) {
            condition_.wait(lock, [this] {
                return stopped_ || interrupted_ || deadline_.has_value();
            });
            continue;
        }

        const auto awaited_deadline = *deadline_;
        condition_.wait_until(lock, awaited_deadline, [this, awaited_deadline] {
            return stopped_ || interrupted_ || !deadline_
                || *deadline_ < awaited_deadline;
        });
    }
}

void Scheduler::interrupt()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        return;
    }
    interrupted_ = true;
    condition_.notify_one();
}

void Scheduler::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    interrupted_ = false;
    deadline_.reset();
    condition_.notify_all();
}

} // namespace gaudere::scheduling::wake
