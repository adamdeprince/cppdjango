// Lower Query → SQL + param order (pure C++, no Python).
#pragma once

#include "orm_engine/query.hpp"

#include <string>
#include <vector>

namespace django::orm {

struct CompiledSql {
  std::string sql;
  // Indices into Query::params in left-to-right placeholder order.
  std::vector<std::uint32_t> param_order;
};

// Returns empty sql on failure (unknown model, etc.).
[[nodiscard]] CompiledSql compile_select(const Query& q,
                                         const SchemaRegistry& reg);

[[nodiscard]] std::string quote_ident(DialectId d, std::string_view name);
[[nodiscard]] DialectId dialect_from_vendor(std::string_view vendor);

}  // namespace django::orm
