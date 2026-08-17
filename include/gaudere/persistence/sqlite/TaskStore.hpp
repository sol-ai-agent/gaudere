#ifndef GAUDERE_PERSISTENCE_SQLITE_TASK_STORE_HPP
#define GAUDERE_PERSISTENCE_SQLITE_TASK_STORE_HPP

#include <gaudere/work/TaskStore.hpp>

#include <string>

struct sqlite3;

namespace gaudere::persistence::sqlite {

class TaskStore final : public work::TaskStore {
public:
    explicit TaskStore(const std::string& path);
    ~TaskStore() override;

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    [[nodiscard]] std::optional<work::Task> find(
        const std::string& id) const override;
    [[nodiscard]] std::optional<work::Task> find_by_idempotency_key(
        const std::string& key) const override;
    [[nodiscard]] std::optional<work::Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const override;
    [[nodiscard]] std::vector<work::Task> leased_with_expired_lease(
        work::TimePoint now) const override;
    [[nodiscard]] std::optional<work::TimePoint> next_lease_expiry() const override;
    [[nodiscard]] bool has_active() const override;
    void save(const work::Task& task) override;

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere::persistence::sqlite

#endif
