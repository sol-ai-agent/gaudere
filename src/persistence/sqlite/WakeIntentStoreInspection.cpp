#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace gaudere::persistence::sqlite {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

void bind_text(sqlite3* database,
               sqlite3_stmt* statement,
               const int index,
               const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

} // namespace

scheduling::wake::WakeIntentScopeInspection WakeIntentStore::inspect_scope(
    const std::string& scope) const
{
    using scheduling::wake::WakeIntentScopeInspection;
    using scheduling::wake::WakeIntentScopeResult;

    if (!scheduling::wake::valid_wake_intent_identifier(scope)) {
        throw std::invalid_argument("invalid wake-intent scope");
    }

    Statement statement(database_,
        "SELECT id FROM wake_intents WHERE scope=?1 ORDER BY id ASC LIMIT 2");
    bind_text(database_, statement.get(), 1, scope);

    const int first_result = sqlite3_step(statement.get());
    if (first_result == SQLITE_DONE) {
        return WakeIntentScopeInspection{WakeIntentScopeResult::empty, std::nullopt};
    }
    if (first_result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    const auto first_id = text(statement.get(), 0);
    const auto first = find(scope, first_id);
    if (!first) {
        throw std::runtime_error("wake-intent scope inspection lost first record");
    }

    const int second_result = sqlite3_step(statement.get());
    if (second_result == SQLITE_DONE) {
        return WakeIntentScopeInspection{WakeIntentScopeResult::one, first};
    }
    if (second_result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    // Validate the second selected row through the same canonical read path too.
    // The result deliberately carries no arbitrary record when scope is ambiguous.
    const auto second_id = text(statement.get(), 0);
    if (!find(scope, second_id)) {
        throw std::runtime_error("wake-intent scope inspection lost second record");
    }
    return WakeIntentScopeInspection{WakeIntentScopeResult::ambiguous, std::nullopt};
}

} // namespace gaudere::persistence::sqlite
