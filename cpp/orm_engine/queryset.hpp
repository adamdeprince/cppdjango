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

struct QNode {
  int kind = 0;  // 0=and 1=or 2=not 3=atom 4=xor
  std::string key;
  std::vector<ParamValue> values;
  std::vector<QNode> children;
  // Atom with SQL RHS (subquery): key + rhs_sql + rhs_params
  bool has_rhs_sql = false;
  std::string rhs_sql;
  std::vector<ParamValue> rhs_params;
  int rhs_op = 0;  // CmpOp
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

  bool apply_q(const QNode& node);

  // field IN (subquery_sql) / field = (subquery_sql)
  bool filter_subquery(std::string_view field_name, CmpOp op,
                       std::string subquery_sql,
                       std::vector<ParamValue> subquery_params);

  bool values(const std::vector<std::string>& field_names, bool out_aliases,
              ResultMode mode);

  // Drop select list / related selects (keep WHERE/JOINs) for pure aggregates.
  void clear_select();

  // Ensure base model concrete columns are in select (for model fetch).
  bool select_model_columns();

  // Aggregate annotation: annotate(alias=Count('field')) or Count(*)
  bool annotate_aggregate(std::string alias, std::string func,
                          std::string field_name, bool distinct, bool star);

  // Scalar subquery annotation: annotate(alias=(SELECT ...))
  bool annotate_sql(std::string alias, std::string sql_fragment,
                    std::vector<ParamValue> params);

  // GROUP BY field names (base model) and/or annotation aliases
  bool group_by_fields(const std::vector<std::string>& field_names);
  void group_by_selected_columns();

  // select_related: LEFT OUTER JOIN + append related concrete columns
  bool add_select_related(std::string_view path);

  // prefetch_related: record lookup for post-fetch
  void add_prefetch(std::string_view lookup);

  bool add_update(std::string_view field_name, ParamValue value);
  bool add_update_null(std::string_view field_name);
  void set_delete();

  bool order_by(std::string_view field_name, bool desc);
  bool order_by_alias(std::string_view alias, bool desc);

  void set_limit(std::uint64_t n);
  void set_offset(std::uint64_t n);
  void clear_limits();
  void set_distinct(bool v);

  [[nodiscard]] CompiledSql compile(
      const SchemaRegistry& reg = SchemaRegistry::instance()) const;

  // Materialize helpers for Python
  [[nodiscard]] std::vector<std::string> base_attnames() const {
    return query_.base_attnames;
  }
  [[nodiscard]] const std::vector<RelatedSelect>& related_selects() const {
    return query_.related_selects;
  }
  [[nodiscard]] const std::vector<PrefetchSpec>& prefetches() const {
    return query_.prefetches;
  }

  [[nodiscard]] QuerySet clone() const;

 private:
  Query query_{};
  std::uint32_t join_counter_ = 0;
  std::unordered_map<std::string, std::string> join_aliases_;

  [[nodiscard]] std::optional<FieldId> resolve_field(std::string_view name) const;
  bool append_pred(Pred p);
  bool resolve_lookup_key(std::string_view key, ColumnRef& col, CmpOp& op,
                          bool& is_isnull_lookup);
  [[nodiscard]] std::optional<BoolExpr> lower_q_node(const QNode& node);
  [[nodiscard]] std::optional<Pred> pred_from_key_values(
      std::string_view key, const std::vector<ParamValue>& values);
  [[nodiscard]] std::optional<Pred> pred_from_q_atom(const QNode& node);

  std::string ensure_join(const ModelSchema& local_model,
                          std::string_view local_alias, std::string_view fk_name,
                          std::string_view path_prefix,
                          JoinType join_type = JoinType::Inner);

  // Join path with LOUTER for select_related; returns alias of final hop.
  std::string ensure_path_joins(std::string_view path, JoinType join_type,
                                const ModelSchema** out_model);
};

}  // namespace django::orm
