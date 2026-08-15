#ifndef GAUDERE_SCHEDULING_WAKE_ACTION_HPP
#define GAUDERE_SCHEDULING_WAKE_ACTION_HPP

#include <chrono>
#include <optional>
#include <string>

namespace gaudere::scheduling::wake {

using TimePoint = std::chrono::system_clock::time_point;

enum class ActionStatus {
    pending,
    running,
    retry_wait,
    succeeded,
    failed_permanent,
    manual_review
};

enum class EffectResult {
    none,
    confirmed,
    unknown
};

struct Lease {
    std::string owner;
    TimePoint expires_at;
};

struct Action {
    std::string id;
    std::string idempotency_key;
    bool critical = false;
    ActionStatus status = ActionStatus::pending;
    EffectResult effect_result = EffectResult::none;
    std::optional<Lease> lease;
};

[[nodiscard]] bool can_transition(ActionStatus from, ActionStatus to) noexcept;

} // namespace gaudere::scheduling::wake

#endif
