#include <gaudere/scheduling/wake/Runtime.hpp>

#include <utility>

namespace gaudere::scheduling::wake {

bool can_transition(const ActionStatus from, const ActionStatus to) noexcept
{
    switch (from) {
    case ActionStatus::pending:
        return to == ActionStatus::running || to == ActionStatus::failed_permanent;
    case ActionStatus::running:
        return to == ActionStatus::retry_wait || to == ActionStatus::succeeded
            || to == ActionStatus::failed_permanent || to == ActionStatus::manual_review;
    case ActionStatus::retry_wait:
        return to == ActionStatus::running || to == ActionStatus::failed_permanent;
    case ActionStatus::succeeded:
    case ActionStatus::failed_permanent:
    case ActionStatus::manual_review:
        return false;
    }
    return false;
}

Runtime::Runtime(ActionStore& store, Now now)
    : store_(store), now_(std::move(now))
{
}

RuntimeState Runtime::state() const noexcept
{
    return state_;
}

void Runtime::recover()
{
    for (auto action : store_.running_with_expired_lease(now_())) {
        action.lease.reset();
        action.status = action.effect_result == EffectResult::unknown
            ? ActionStatus::manual_review
            : ActionStatus::retry_wait;
        store_.save(action);
    }
    state_ = RuntimeState::running;
}

SubmitResult Runtime::submit(const Action& action)
{
    if (state_ != RuntimeState::running && state_ != RuntimeState::draining) {
        return SubmitResult::unavailable;
    }
    if (state_ == RuntimeState::draining && action.critical) {
        return SubmitResult::critical_rejected;
    }
    if (store_.find_by_idempotency_key(action.idempotency_key)) {
        return SubmitResult::duplicate;
    }
    store_.save(action);
    return SubmitResult::accepted;
}

bool Runtime::start(const std::string& id,
                    std::string lease_owner,
                    const Duration lease_duration)
{
    if (state_ != RuntimeState::running) {
        return false;
    }
    auto action = store_.find(id);
    if (!action || !can_transition(action->status, ActionStatus::running)) {
        return false;
    }
    action->status = ActionStatus::running;
    action->lease = Lease{std::move(lease_owner), now_() + lease_duration};
    store_.save(*action);
    return true;
}

bool Runtime::transition(const std::string& id, const ActionStatus status)
{
    auto action = store_.find(id);
    if (!action || !can_transition(action->status, status)) {
        return false;
    }
    action->status = status;
    if (status != ActionStatus::running) {
        action->lease.reset();
    }
    store_.save(*action);
    return true;
}

bool Runtime::record_unknown_result(const std::string& id)
{
    auto action = store_.find(id);
    if (!action || action->status != ActionStatus::running) {
        return false;
    }
    action->effect_result = EffectResult::unknown;
    action->status = ActionStatus::manual_review;
    action->lease.reset();
    store_.save(*action);
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
    if (state_ != RuntimeState::draining || store_.has_running()) {
        return false;
    }
    state_ = RuntimeState::safe;
    return true;
}

} // namespace gaudere::scheduling::wake
