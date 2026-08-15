#ifndef GAUDERE_SCHEDULING_WAKE_RUNTIME_HPP
#define GAUDERE_SCHEDULING_WAKE_RUNTIME_HPP

#include <gaudere/scheduling/wake/ActionStore.hpp>

#include <chrono>
#include <functional>
#include <string>

namespace gaudere::scheduling::wake {

enum class RuntimeState {
    recovering,
    running,
    draining,
    safe
};

enum class SubmitResult {
    accepted,
    duplicate,
    critical_rejected,
    unavailable
};

class Runtime {
public:
    using Duration = std::chrono::system_clock::duration;
    using Now = std::function<TimePoint()>;

    explicit Runtime(ActionStore& store, Now now);

    [[nodiscard]] RuntimeState state() const noexcept;
    void recover();
    [[nodiscard]] SubmitResult submit(const Action& action);
    [[nodiscard]] bool start(const std::string& id,
                             std::string lease_owner,
                             Duration lease_duration);
    [[nodiscard]] bool transition(const std::string& id, ActionStatus status);
    [[nodiscard]] bool record_unknown_result(const std::string& id);
    void request_shutdown() noexcept;
    [[nodiscard]] bool try_mark_safe();

private:
    ActionStore& store_;
    Now now_;
    RuntimeState state_ = RuntimeState::recovering;
};

} // namespace gaudere::scheduling::wake

#endif
