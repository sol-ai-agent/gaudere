#ifndef GAUDERE_BUDGET_STORE_HPP
#define GAUDERE_BUDGET_STORE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere::budget {

using TimePoint = std::chrono::system_clock::time_point;

struct Policy {
    std::uint64_t max_total = 0;
    std::uint64_t max_in_window = 0;
    std::chrono::milliseconds window{0};
    std::chrono::milliseconds min_interval{0};
};

enum class ConsumeResult {
    accepted,
    duplicate,
    total_exhausted,
    window_exhausted,
    cooldown,
    clock_rollback
};

struct Snapshot {
    std::uint64_t total_used = 0;
    std::uint64_t in_window_used = 0;
    std::optional<TimePoint> last_consumed_at;
    ConsumeResult next_new_consumption = ConsumeResult::accepted;
};

[[nodiscard]] inline bool valid_policy(const Policy& policy) noexcept
{
    return policy.max_total > 0
        && policy.max_in_window > 0
        && policy.max_in_window <= policy.max_total
        && policy.window.count() > 0
        && policy.min_interval.count() >= 0
        && policy.min_interval <= policy.window;
}

/**
 * Durable admission boundary for scarce effects.
 *
 * consume() must atomically check the policy and persist a successful consumption.
 * Reusing the same (scope, idempotency_key) is idempotent and returns duplicate,
 * even after limits have otherwise been exhausted.
 *
 * snapshot() is observational only. It reports durable usage and the result that a
 * hypothetical brand-new idempotency key would receive at `now`; it never returns
 * duplicate and never consumes a permit.
 */
class Store {
public:
    virtual ~Store() = default;

    [[nodiscard]] virtual ConsumeResult consume(
        const std::string& scope,
        const std::string& idempotency_key,
        TimePoint now,
        const Policy& policy) = 0;

    [[nodiscard]] virtual Snapshot snapshot(
        const std::string& scope,
        TimePoint now,
        const Policy& policy) = 0;
};

} // namespace gaudere::budget

#endif
