#ifndef GAUDERE_WORK_TASK_STORE_HPP
#define GAUDERE_WORK_TASK_STORE_HPP

#include <gaudere/work/Task.hpp>

#include <optional>
#include <string>
#include <vector>

namespace gaudere::work {

class TaskStore {
public:
    virtual ~TaskStore() = default;

    [[nodiscard]] virtual std::optional<Task> find(const std::string& id) const = 0;
    [[nodiscard]] virtual std::optional<Task> find_by_idempotency_key(
        const std::string& key) const = 0;

    /** Return one pending task whose kind is accepted by the caller.
     *
     * Selection does not claim, lease, or mutate the task. The current Gaudere
     * runtime model has one process owning a state database; a future
     * multi-worker model will require an atomic claim contract instead.
     */
    [[nodiscard]] virtual std::optional<Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const = 0;

    [[nodiscard]] virtual std::vector<Task> leased_with_expired_lease(
        TimePoint now) const = 0;
    [[nodiscard]] virtual bool has_active() const = 0;
    virtual void save(const Task& task) = 0;
};

} // namespace gaudere::work

#endif
