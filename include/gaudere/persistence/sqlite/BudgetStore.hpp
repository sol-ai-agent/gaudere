#ifndef GAUDERE_PERSISTENCE_SQLITE_BUDGET_STORE_HPP
#define GAUDERE_PERSISTENCE_SQLITE_BUDGET_STORE_HPP

#include <gaudere/budget/Store.hpp>

#include <string>

struct sqlite3;

namespace gaudere::persistence::sqlite {

class BudgetStore final : public budget::Store {
public:
    explicit BudgetStore(const std::string& path);
    ~BudgetStore() override;

    BudgetStore(const BudgetStore&) = delete;
    BudgetStore& operator=(const BudgetStore&) = delete;

    [[nodiscard]] budget::ConsumeResult consume(
        const std::string& scope,
        const std::string& idempotency_key,
        budget::TimePoint now,
        const budget::Policy& policy) override;

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere::persistence::sqlite

#endif
