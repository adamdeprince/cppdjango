// C++ QuerySet: mutates Query graph; compile is pure C++.
#pragma once

#include "orm_engine/compile.hpp"
#include "orm_engine/query.hpp"
#include "orm_engine/schema.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace django::orm {

struct FilterResult {
  bool ok = false;
  std::string error;
};

class QuerySet {
 public:
  QuerySet() = default;
  explicit QuerySet(ModelId model, DialectId dialect = DialectId::Postgres);

  [[nodiscard]] ModelId model_id() const { return query_.model; }
  [[nodiscard]] const Query& query() const { return query_; }
  Query& query_mut() { return query_; }

  bool filter_eq(std::string_view field_name, ParamValue value);
  bool filter_in(std::string_view field_name, std::vector<ParamValue> values);
  bool filter_cmp(std::string_view field_name, CmpOp op, ParamValue value);
  bool filter_isnull(std::string_view field_name, bool is_null);

  // One-shot kwargs: list of (key, value) or (key, list_values for IN).
  // key is "field" or "field__lookup" or "fk__field__exact".
  // Returns false if any key cannot be applied (partial apply is rolled back
  // by caller cloning first).
  bool filter_kwargs(
      const std::vector<std::pair<std::string, std::vector<ParamValue>>>& items,
      bool disjunctive);

  bool values(const std::vector<std::string>& field_names, bool out_aliases,
              ResultMode mode);

  // UPDATE SET field=value (params). Switches kind to Update.
  bool add_update(std::string_view field_name, ParamValue value);
  bool add_update_null(std::string_view field_name);
  void set_delete();

  bool order_by(std::string_view field_name, bool desc);

  void set_limit(std::uint64_t n);
  void set_offset(std::uint64_t n);
  void clear_limits();

  [[nodiscard]] CompiledSql compile(
      const SchemaRegistry& reg = SchemaRegistry::instance()) const;

  [[nodiscard]] QuerySet clone() const { return *this; }

 private:
  Query query_{};
  std::uint32_t join_counter_ = 0;
  // path prefix → join alias
  std::unordered_map<std::string, std::string> join_aliases_;

  [[nodiscard]] std::optional<FieldId> resolve_field(std::string_view name) const;
  bool append_pred(Pred p);
  // Resolve "a__b__lookup" → column ref (+ joins) and CmpOp.
  bool resolve_lookup_key(std::string_view key, ColumnRef& col, CmpOp& op,
                          bool& is_isnull_lookup);
};

}  // namespace django::orm
