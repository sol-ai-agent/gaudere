#ifndef GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_HPP
#define GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere::scheduling::wake {

using WakeIntentTimePoint = std::chrono::system_clock::time_point;

inline constexpr std::size_t wake_intent_identifier_max_bytes = 128;
inline constexpr std::size_t wake_intent_reason_max_bytes = 1024;

enum class WakeIntentStatus {
    scheduled,
    fired,
    revoked,
    manual_review
};

struct WakeIntentPolicy {
    std::uint64_t max_total = 0;
};

struct WakeIntent {
    std::string scope;
    std::string id;
    std::string source_id;
    WakeIntentTimePoint accepted_at;
    WakeIntentTimePoint due_at;
    WakeIntentStatus status = WakeIntentStatus::scheduled;
    std::optional<WakeIntentTimePoint> terminal_at;
    std::string terminal_reason;
};

enum class WakeIntentScopeResult {
    empty,
    one,
    ambiguous
};

struct WakeIntentScopeInspection {
    WakeIntentScopeResult result = WakeIntentScopeResult::empty;
    std::optional<WakeIntent> intent;
};

enum class WakeIntentAcceptResult {
    accepted,
    duplicate,
    total_exhausted,
    conflict,
    invalid
};

enum class WakeIntentRevokeResult {
    revoked,
    fired,
    manual_review,
    not_found,
    terminal,
    invalid
};

struct WakeIntentReconcileResult {
    std::size_t fired = 0;
    std::size_t manual_review = 0;
};

[[nodiscard]] bool is_terminal(WakeIntentStatus status) noexcept;
[[nodiscard]] bool valid_wake_intent_identifier(
    const std::string& value) noexcept;
[[nodiscard]] bool valid_wake_intent_policy(
    const WakeIntentPolicy& policy) noexcept;
[[nodiscard]] bool valid_wake_intent(const WakeIntent& intent) noexcept;
[[nodiscard]] bool valid_new_wake_intent(const WakeIntent& intent) noexcept;

} // namespace gaudere::scheduling::wake

#endif
