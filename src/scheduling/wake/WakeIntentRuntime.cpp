#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gaudere::scheduling::wake {
namespace {

using Milliseconds = std::chrono::milliseconds;

bool millisecond_aligned(const WakeIntentTimePoint value) noexcept
{
    return std::chrono::duration_cast<Milliseconds>(value.time_since_epoch())
        == value.time_since_epoch();
}

bool valid_terminal_shape(const WakeIntent& intent) noexcept
{
    switch (intent.status) {
    case WakeIntentStatus::scheduled:
        return !intent.terminal_at && intent.terminal_reason.empty();
    case WakeIntentStatus::fired:
        return intent.terminal_at && *intent.terminal_at >= intent.due_at
            && intent.terminal_reason.empty();
    case WakeIntentStatus::revoked:
        return intent.terminal_at && *intent.terminal_at >= intent.accepted_at
            && *intent.terminal_at < intent.due_at
            && !intent.terminal_reason.empty();
    case WakeIntentStatus::manual_review:
        return intent.terminal_at && !intent.terminal_reason.empty();
    }
    return false;
}

} // namespace

bool is_terminal(const WakeIntentStatus status) noexcept
{
    return status == WakeIntentStatus::fired
        || status == WakeIntentStatus::revoked
        || status == WakeIntentStatus::manual_review;
}

bool valid_wake_intent_identifier(const std::string& value) noexcept
{
    return !value.empty() && value.size() <= wake_intent_identifier_max_bytes;
}

bool valid_wake_intent_policy(const WakeIntentPolicy& policy) noexcept
{
    return policy.max_total > 0
        && policy.max_total
            <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

bool valid_wake_intent(const WakeIntent& intent) noexcept
{
    if (!valid_wake_intent_identifier(intent.scope)
        || !valid_wake_intent_identifier(intent.id)
        || !valid_wake_intent_identifier(intent.source_id)
        || !millisecond_aligned(intent.accepted_at)
        || !millisecond_aligned(intent.due_at)
        || intent.due_at <= intent.accepted_at
        || intent.terminal_reason.size() > wake_intent_reason_max_bytes
        || !valid_terminal_shape(intent)) {
        return false;
    }
    return !intent.terminal_at || millisecond_aligned(*intent.terminal_at);
}

bool valid_new_wake_intent(const WakeIntent& intent) noexcept
{
    return valid_wake_intent(intent)
        && intent.status == WakeIntentStatus::scheduled;
}

WakeIntentRuntime::WakeIntentRuntime(WakeIntentStore& store,
                                     Now now,
                                     std::string scope,
                                     WakeIntentPolicy policy)
    : store_(store),
      now_(std::move(now)),
      scope_(std::move(scope)),
      policy_(policy)
{
    if (!now_ || !valid_wake_intent_identifier(scope_)
        || !valid_wake_intent_policy(policy_)) {
        throw std::invalid_argument("invalid wake-intent runtime configuration");
    }
}

WakeIntentTimePoint WakeIntentRuntime::now_milliseconds() const
{
    return std::chrono::floor<Milliseconds>(now_());
}

WakeIntentAcceptResult WakeIntentRuntime::accept(
    std::string id,
    std::string source_id,
    const std::chrono::milliseconds delay)
{
    if (!valid_wake_intent_identifier(id)
        || !valid_wake_intent_identifier(source_id)
        || delay.count() <= 0) {
        return WakeIntentAcceptResult::invalid;
    }

    const auto accepted_at = now_milliseconds();
    const auto remaining = std::chrono::duration_cast<Milliseconds>(
        WakeIntentTimePoint::max() - accepted_at);
    if (delay > remaining) {
        return WakeIntentAcceptResult::invalid;
    }
    const auto system_delay =
        std::chrono::duration_cast<WakeIntentTimePoint::duration>(delay);
    if (system_delay <= WakeIntentTimePoint::duration::zero()
        || accepted_at > WakeIntentTimePoint::max() - system_delay) {
        return WakeIntentAcceptResult::invalid;
    }

    WakeIntent intent;
    intent.scope = scope_;
    intent.id = std::move(id);
    intent.source_id = std::move(source_id);
    intent.accepted_at = accepted_at;
    intent.due_at = accepted_at + system_delay;
    if (!valid_new_wake_intent(intent)) {
        return WakeIntentAcceptResult::invalid;
    }
    return store_.accept(intent, policy_);
}

WakeIntentReconcileResult WakeIntentRuntime::reconcile()
{
    return store_.reconcile(scope_, now_milliseconds());
}

WakeIntentRevokeResult WakeIntentRuntime::revoke(
    const std::string& id,
    std::string reason)
{
    if (!valid_wake_intent_identifier(id) || reason.empty()
        || reason.size() > wake_intent_reason_max_bytes) {
        return WakeIntentRevokeResult::invalid;
    }
    return store_.revoke(scope_, id, now_milliseconds(), reason);
}

std::optional<WakeIntent> WakeIntentRuntime::find(const std::string& id) const
{
    if (!valid_wake_intent_identifier(id)) {
        return std::nullopt;
    }
    return store_.find(scope_, id);
}

std::optional<WakeIntentTimePoint> WakeIntentRuntime::next_scheduled_at() const
{
    return store_.next_scheduled_at(scope_);
}

const std::string& WakeIntentRuntime::scope() const noexcept
{
    return scope_;
}

const WakeIntentPolicy& WakeIntentRuntime::policy() const noexcept
{
    return policy_;
}

} // namespace gaudere::scheduling::wake
