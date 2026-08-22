#ifndef GAUDERE_PERSISTENCE_SQLITE_WAKE_INTENT_STORE_HPP
#define GAUDERE_PERSISTENCE_SQLITE_WAKE_INTENT_STORE_HPP

#include <gaudere/scheduling/wake/WakeIntentStore.hpp>

#include <string>

struct sqlite3;

namespace gaudere::persistence::sqlite {

class WakeIntentStore final : public scheduling::wake::WakeIntentStore {
public:
    explicit WakeIntentStore(const std::string& path);
    ~WakeIntentStore() override;

    WakeIntentStore(const WakeIntentStore&) = delete;
    WakeIntentStore& operator=(const WakeIntentStore&) = delete;

    [[nodiscard]] std::optional<scheduling::wake::WakeIntent> find(
        const std::string& scope,
        const std::string& id) const override;
    [[nodiscard]] std::optional<scheduling::wake::WakeIntent> find_by_source(
        const std::string& scope,
        const std::string& source_id) const override;
    [[nodiscard]] scheduling::wake::WakeIntentAcceptResult accept(
        const scheduling::wake::WakeIntent& intent,
        const scheduling::wake::WakeIntentPolicy& policy) override;
    [[nodiscard]] std::optional<scheduling::wake::WakeIntentTimePoint>
        next_scheduled_at(const std::string& scope) const override;
    [[nodiscard]] scheduling::wake::WakeIntentReconcileResult reconcile(
        const std::string& scope,
        scheduling::wake::WakeIntentTimePoint now) override;
    [[nodiscard]] scheduling::wake::WakeIntentRevokeResult revoke(
        const std::string& scope,
        const std::string& id,
        scheduling::wake::WakeIntentTimePoint now,
        const std::string& reason) override;

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere::persistence::sqlite

#endif
