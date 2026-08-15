#ifndef GAUDERE_PERSISTENCE_SQLITE_ACTION_STORE_HPP
#define GAUDERE_PERSISTENCE_SQLITE_ACTION_STORE_HPP

#include <gaudere/scheduling/wake/ActionStore.hpp>

#include <string>

struct sqlite3;

namespace gaudere::persistence::sqlite {

/** SQLite-backed wake ActionStore.
 *
 * Time points are stored as signed 64-bit milliseconds since the Unix epoch.
 */
class ActionStore final : public scheduling::wake::ActionStore {
public:
    explicit ActionStore(const std::string& path);
    ~ActionStore() override;

    ActionStore(const ActionStore&) = delete;
    ActionStore& operator=(const ActionStore&) = delete;

    [[nodiscard]] std::optional<scheduling::wake::Action> find(
        const std::string& id) const override;
    [[nodiscard]] std::optional<scheduling::wake::Action> find_by_idempotency_key(
        const std::string& key) const override;
    [[nodiscard]] std::vector<scheduling::wake::Action> running_with_expired_lease(
        scheduling::wake::TimePoint now) const override;
    [[nodiscard]] bool has_running() const override;
    void save(const scheduling::wake::Action& action) override;

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere::persistence::sqlite

#endif
