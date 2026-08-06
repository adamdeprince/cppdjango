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

// Serialized Q-tree node (from Python Q or kwargs).
// kind: 0=And, 1=Or, 2=Not, 3=Atom (key + values)
struct QNode {
  int kind = 0;
  std::string key;
  std::vector<ParamValue> values;
  std::vector<QNode> children;
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

  bool filter_kwargs(
      const std::vector<std::pair<std::string, std::vector<ParamValue>>>& items,
      bool disjunctive);

  // Full Q tree (AND/OR/NOT/atoms). One call, pure C++ graph mutation.
  bool apply_q(const QNode& node);

  bool values(const std::vector<std::string>& field_names, bool out_aliases,
              ResultMode mode);

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
  // path prefix "a" / "a__b" → join alias
  std::unordered_map<std::string, std::string> join_aliases_;

  [[nodiscard]] std::optional<FieldId> resolve_field(std::string_view name) const;
  bool append_pred(Pred p);
  bool resolve_lookup_key(std::string_view key, ColumnRef& col, CmpOp& op,
                          bool& is_isnull_lookup);
  // Build BoolExpr from QNode without attaching (for nesting).
  [[nodiscard]] std::optional<BoolExpr> lower_q_node(const QNode& node);
  [[nodiscard]] std::optional<Pred> pred_from_key_values(
      std::string_view key, const std::vector<ParamValue>& values);

  // Ensure join for path_prefix ending at FK field on `local` model/alias.
  // Returns join alias for the remote table, or empty on failure.
  std::string ensure_join(const ModelSchema& local_model,
                          std::string_view local_alias, std::string_view fk_name,
                          std::string_view path_prefix);
};

}  // namespace django::orm
