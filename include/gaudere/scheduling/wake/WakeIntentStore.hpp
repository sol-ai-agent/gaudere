#ifndef GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_STORE_HPP
#define GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_STORE_HPP

#include <gaudere/scheduling/wake/WakeIntent.hpp>

#include <optional>
#include <string>

namespace gaudere::scheduling::wake {

enum class WakeIntentScopeResult {
    empty,
    one,
    ambiguous
};

struct WakeIntentScopeInspection {
    WakeIntentScopeResult result = WakeIntentScopeResult::empty;
    std::optional<WakeIntent> intent;
};

/** Durable atomic boundary for exact wake intents.
 *
 * accept() checks the per-scope lifetime policy and inserts in one transaction.
 * reconcile() and revoke() atomically perform the only permitted terminal
 * transitions. Implementations must never delete or recycle an accepted row.
 */
class WakeIntentStore {
public:
    virtual ~WakeIntentStore() = default;

    [[nodiscard]] virtual std::optional<WakeIntent> find(
        const std::string& scope,
        const std::string& id) const = 0;
    [[nodiscard]] virtual std::optional<WakeIntent> find_by_source(
        const std::string& scope,
        const std::string& source_id) const = 0;
    /** Inspect one fixed scope without exposing an unbounded list.
     *
     * Implementations inspect at most two records and distinguish zero, exactly
     * one, and ambiguity. No arbitrary record may be selected when two or more
     * records exist.
     */
    [[nodiscard]] virtual WakeIntentScopeInspection inspect_scope(
        const std::string& scope) const = 0;
    [[nodiscard]] virtual WakeIntentAcceptResult accept(
        const WakeIntent& intent,
        const WakeIntentPolicy& policy) = 0;
    [[nodiscard]] virtual std::optional<WakeIntentTimePoint> next_scheduled_at(
        const std::string& scope) const = 0;
    [[nodiscard]] virtual WakeIntentReconcileResult reconcile(
        const std::string& scope,
        WakeIntentTimePoint now) = 0;
    [[nodiscard]] virtual WakeIntentRevokeResult revoke(
        const std::string& scope,
        const std::string& id,
        WakeIntentTimePoint now,
        const std::string& reason) = 0;
};

} // namespace gaudere::scheduling::wake

#endif
