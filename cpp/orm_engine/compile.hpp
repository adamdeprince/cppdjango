// Lower Query → SQL + param order (pure C++, no Python).
#pragma once

#include "orm_engine/query.hpp"

#include <string>
#include <vector>

namespace django::orm {

struct CompiledSql {
  std::string sql;
  std::vector<std::uint32_t> param_order;
};

[[nodiscard]] CompiledSql compile_query(const Query& q,
                                        const SchemaRegistry& reg);

// Back-compat alias
[[nodiscard]] inline CompiledSql compile_select(const Query& q,
                                                const SchemaRegistry& reg) {
  return compile_query(q, reg);
}

// Emit a boolean expression SQL fragment (for CASE WHEN, etc.).
// Appends placeholder order indices into `order` (indexes into q.params).
void append_bool_sql(std::string& out, std::vector<std::uint32_t>& order,
                     const Query& q, const ModelSchema& m, const BoolExpr& e);

// Emit a column reference.
void append_column_sql(std::string& out, DialectId d, const Query& q,
                       const ModelSchema& m, const ColumnRef& c);

[[nodiscard]] std::string quote_ident(DialectId d, std::string_view name);
[[nodiscard]] DialectId dialect_from_vendor(std::string_view vendor);

}  // namespace django::orm
