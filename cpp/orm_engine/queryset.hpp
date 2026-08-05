// C++ QuerySet: mutates Query graph; compile is pure C++.
#pragma once

#include "orm_engine/compile.hpp"
#include "orm_engine/query.hpp"
#include "orm_engine/schema.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::orm {

class QuerySet {
 public:
  QuerySet() = default;
  explicit QuerySet(ModelId model, DialectId dialect = DialectId::Postgres);

  [[nodiscard]] ModelId model_id() const { return query_.model; }
  [[nodiscard]] const Query& query() const { return query_; }
  Query& query_mut() { return query_; }

  // Lookup name: field name, attname, or "pk". Value as ParamValue.
  // Returns false if field unknown (caller falls back).
  bool filter_eq(std::string_view field_name, ParamValue value);
  bool filter_in(std::string_view field_name, std::vector<ParamValue> values);
  bool filter_cmp(std::string_view field_name, CmpOp op, ParamValue value);

  // Restrict select list to named fields (values/values_list).
  // out_aliases: if true, AS field_name for each.
  bool values(const std::vector<std::string>& field_names, bool out_aliases,
              ResultMode mode);

  void set_limit(std::uint64_t n);
  void set_offset(std::uint64_t n);
  void clear_limits();

  [[nodiscard]] CompiledSql compile(const SchemaRegistry& reg = SchemaRegistry::instance()) const;

  // Clone for chain semantics.
  [[nodiscard]] QuerySet clone() const { return *this; }

 private:
  Query query_{};
  [[nodiscard]] std::optional<FieldId> resolve_field(std::string_view name) const;
};

}  // namespace django::orm
