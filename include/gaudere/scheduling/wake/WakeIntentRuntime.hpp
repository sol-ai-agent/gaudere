#ifndef GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_RUNTIME_HPP
#define GAUDERE_SCHEDULING_WAKE_WAKE_INTENT_RUNTIME_HPP

#include <gaudere/scheduling/wake/WakeIntentStore.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace gaudere::scheduling::wake {

/** Provider-free coordinator for one application-selected wake capability.
 *
 * The application fixes scope and policy at construction. accept() samples the
 * injected clock once, derives an immutable millisecond deadline, and asks the
 * store to consume the durable per-scope slot before returning success.
 */
class WakeIntentRuntime {
public:
    using Now = std::function<WakeIntentTimePoint()>;

    WakeIntentRuntime(WakeIntentStore& store,
                      Now now,
                      std::string scope,
                      WakeIntentPolicy policy);

    [[nodiscard]] WakeIntentAcceptResult accept(
        std::string id,
        std::string source_id,
        std::chrono::milliseconds delay);
    [[nodiscard]] WakeIntentReconcileResult reconcile();
    [[nodiscard]] WakeIntentRevokeResult revoke(
        const std::string& id,
        std::string reason);
    [[nodiscard]] std::optional<WakeIntent> find(
        const std::string& id) const;
    [[nodiscard]] WakeIntentScopeInspection inspect_scope() const;
    [[nodiscard]] std::optional<WakeIntentTimePoint> next_scheduled_at() const;

    [[nodiscard]] const std::string& scope() const noexcept;
    [[nodiscard]] const WakeIntentPolicy& policy() const noexcept;

private:
    [[nodiscard]] WakeIntentTimePoint now_milliseconds() const;

    WakeIntentStore& store_;
    Now now_;
    std::string scope_;
    WakeIntentPolicy policy_;
};

} // namespace gaudere::scheduling::wake

#endif
