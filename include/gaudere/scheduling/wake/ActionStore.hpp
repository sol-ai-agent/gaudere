#ifndef GAUDERE_SCHEDULING_WAKE_ACTION_STORE_HPP
#define GAUDERE_SCHEDULING_WAKE_ACTION_STORE_HPP

#include <gaudere/scheduling/wake/Action.hpp>

#include <optional>
#include <string>
#include <vector>

namespace gaudere::scheduling::wake {

class ActionStore {
public:
    virtual ~ActionStore() = default;

    [[nodiscard]] virtual std::optional<Action> find(const std::string& id) const = 0;
    [[nodiscard]] virtual std::optional<Action> find_by_idempotency_key(
        const std::string& key) const = 0;
    [[nodiscard]] virtual std::vector<Action> running_with_expired_lease(
        TimePoint now) const = 0;
    [[nodiscard]] virtual bool has_running() const = 0;
    virtual void save(const Action& action) = 0;
};

} // namespace gaudere::scheduling::wake

#endif
