// C++ QuerySet: mutates Query graph; compile is pure C++.
#pragma once

#include "orm_engine/compile.hpp"
#include "orm_engine/query.hpp"
#include "orm_engine/schema.hpp"

#include <optional>
#include <memory>
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

// Compact, Python-object-free replay state for QuerySet operations that the
// native data plane owns. The Python QuerySet wrapper treats this as an
// immutable value: every ``with_*`` operation returns a new tail, allowing
// cloned Python QuerySets to share earlier nodes without mutating them.
class QueryPlan {
 public:
  enum class OperationKind { Filter, OrderBy, Values };

  struct Operation {
    OperationKind kind = OperationKind::Filter;
    QNode filter;
    std::vector<std::string> names;
    bool simple_filter = false;
    std::string lookup_field;
    bool lookup_in = false;
  };

  struct SimpleValuesShape {
    std::vector<std::string> fields;
    std::vector<std::string> ordering;
    std::string lookup_field;
    bool lookup_in = false;
    const std::vector<ParamValue>* lookup_values = nullptr;
  };

  struct SimpleUpdateShape {
    std::string lookup_field;
    const ParamValue* lookup_value = nullptr;
  };

  QueryPlan() = default;
  explicit QueryPlan(ModelId model);

  [[nodiscard]] bool is_model_bound() const { return model_.has_value(); }
  [[nodiscard]] bool matches_model(ModelId model) const;
  [[nodiscard]] std::optional<ModelId> model_id() const { return model_; }

  [[nodiscard]] QueryPlan with_filter(QNode filter) const;
  [[nodiscard]] QueryPlan with_simple_filter(
      std::string lookup_field, bool lookup_in,
      std::vector<ParamValue> lookup_values) const;
  [[nodiscard]] QueryPlan with_ordering(
      std::vector<std::string> field_names) const;
  [[nodiscard]] QueryPlan with_values(
      std::vector<std::string> field_names) const;

  [[nodiscard]] bool has_only_projection() const;
  [[nodiscard]] bool has_simple_filter() const;
  [[nodiscard]] std::optional<SimpleValuesShape> simple_values_shape() const;
  [[nodiscard]] std::optional<SimpleUpdateShape> simple_update_shape() const;
  // Materialized only for the compatibility replay path.
  [[nodiscard]] std::vector<Operation> operations() const;

 private:
  struct Node {
    Operation operation;
    std::shared_ptr<const Node> previous;
  };

  std::shared_ptr<const Node> tail_;
  std::optional<ModelId> model_;
  std::uint64_t schema_generation_ = 0;

  [[nodiscard]] QueryPlan append(Operation operation) const;
};

class QuerySet {
 public:
  QuerySet();
  explicit QuerySet(ModelId model, DialectId dialect = DialectId::Postgres);

  [[nodiscard]] ModelId model_id() const { return q().model; }
  [[nodiscard]] const Query& query() const { return q(); }
  Query& query_mut() { return q(); }

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
  // All forward FK/O2O paths up to max_depth (unrestricted select_related()).
  bool add_select_related_all(int max_depth = 5);

  // prefetch_related: record lookup (+ schema metadata) for secondary queries
  bool add_prefetch(std::string_view lookup);

  // Nested QuerySet as subquery (compiled entirely in C++).
  bool filter_subquery_qs(std::string_view field_name, CmpOp op,
                          const QuerySet& sub);
  bool annotate_subquery_qs(std::string alias, const QuerySet& sub);

  // CASE WHEN ... THEN ... ELSE ... END annotation (when = QNode trees).
  // each case: (when_tree, then_value); else optional.
  bool annotate_case(
      std::string alias,
      const std::vector<std::pair<QNode, ParamValue>>& cases,
      bool has_else, ParamValue else_value);

  // CombinedExpression-style: field op value  or field op field
  bool annotate_binop(std::string alias, std::string lhs_field,
                      std::string op, ParamValue rhs);
  bool annotate_binop_fields(std::string alias, std::string lhs_field,
                             std::string op, std::string rhs_field);

  // Value() constant annotation
  bool annotate_value(std::string alias, ParamValue value);

  // F('field') annotation (column alias)
  bool annotate_f(std::string alias, std::string field_name);

  // Build secondary SELECT for prefetch given parent PK values.
  // Returns empty sql on failure; params are fully flattened for execute.
  // parent_link_offset >= 0 means that column index holds the parent PK
  // (used for M2M bucketing where the FK is on the through table).
  struct PrefetchSql {
    std::string sql;
    std::vector<ParamValue> params;
    int parent_link_offset = -1;
  };
  [[nodiscard]] PrefetchSql compile_prefetch_secondary(
      const PrefetchSpec& spec,
      const std::vector<ParamValue>& parent_pks) const;

  bool add_update(std::string_view field_name, ParamValue value);
  bool add_update_null(std::string_view field_name);
  void set_delete();

  bool order_by(std::string_view field_name, bool desc);
  bool order_by_alias(std::string_view alias, bool desc);
  void clear_ordering();

  void set_limit(std::uint64_t n);
  void set_offset(std::uint64_t n);
  void clear_limits();
  void set_distinct(bool v);

  [[nodiscard]] CompiledSql compile(
      const SchemaRegistry& reg = SchemaRegistry::instance()) const;

  // Materialize helpers for Python
  [[nodiscard]] std::vector<std::string> base_attnames() const {
    return q().base_attnames;
  }
  [[nodiscard]] const std::vector<RelatedSelect>& related_selects() const {
    return q().related_selects;
  }
  [[nodiscard]] const std::vector<PrefetchSpec>& prefetches() const {
    return q().prefetches;
  }
  // Annotation aliases with select-list offsets for setattr on instances.
  struct AnnotationSelect {
    std::string alias;
    int offset = 0;
  };
  [[nodiscard]] std::vector<AnnotationSelect> annotation_selects() const;

  [[nodiscard]] QuerySet clone() const;
  [[nodiscard]] bool shares_state_with(const QuerySet& other) const {
    return state_ == other.state_;
  }
  [[nodiscard]] std::uint64_t compile_runs() const {
    return state_->compile_runs;
  }

 private:
  struct State {
    Query query{};
    std::uint32_t join_counter = 0;
    std::unordered_map<std::string, std::string> join_aliases;
    mutable std::optional<CompiledSql> compiled;
    mutable std::uint64_t compiled_schema_generation = 0;
    mutable std::uint64_t compile_runs = 0;
  };

  std::shared_ptr<State> state_;

  void detach();
  State& s();
  [[nodiscard]] const State& s() const { return *state_; }
  Query& q() { return s().query; }
  [[nodiscard]] const Query& q() const { return s().query; }

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
