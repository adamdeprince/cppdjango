// Native query graph (authoritative IR for the data plane).
#pragma once

#include "orm_engine/schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::orm {

enum class DialectId : std::uint8_t { Postgres = 0, MySQL, SQLite };

enum class CmpOp : std::uint8_t {
  Eq = 0,
  Ne,
  Lt,
  Lte,
  Gt,
  Gte,
  In,
  IsNull,
};

enum class StmtKind : std::uint8_t { Select = 0, Update, Delete };

enum class JoinType : std::uint8_t { Inner = 0, LeftOuter };

struct ParamValue {
  enum class Kind : std::uint8_t {
    None = 0,
    Bool,
    Int,
    Float,
    String,
    Bytes,
  };
  Kind kind = Kind::None;
  bool b = false;
  std::int64_t i = 0;
  double f = 0;
  std::string s;
  std::vector<std::uint8_t> bytes;

  static ParamValue null() { return {}; }
  static ParamValue from_bool(bool v) {
    ParamValue p;
    p.kind = Kind::Bool;
    p.b = v;
    return p;
  }
  static ParamValue from_int(std::int64_t v) {
    ParamValue p;
    p.kind = Kind::Int;
    p.i = v;
    return p;
  }
  static ParamValue from_float(double v) {
    ParamValue p;
    p.kind = Kind::Float;
    p.f = v;
    return p;
  }
  static ParamValue from_string(std::string v) {
    ParamValue p;
    p.kind = Kind::String;
    p.s = std::move(v);
    return p;
  }
};

struct ColumnRef {
  FieldId field = 0;
  std::string table_alias;
  std::string column_override;
};

struct Pred {
  CmpOp op = CmpOp::Eq;
  ColumnRef lhs{};
  // Optional: RHS is a SQL fragment (subquery) instead of params.
  bool rhs_is_sql = false;
  std::string rhs_sql;  // e.g. "(SELECT ...)" already parenthesized
  std::vector<std::uint32_t> rhs_sql_param_idxs;
  std::vector<std::uint32_t> param_idxs;
  bool is_null_negated = false;
};

struct BoolExpr {
  enum class Kind : std::uint8_t { Atom = 0, And, Or, Not, Xor };
  Kind kind = Kind::Atom;
  Pred atom{};
  std::vector<BoolExpr> children;
};

enum class SelectKind : std::uint8_t {
  Column = 0,
  Aggregate = 1,
  SqlFragment = 2,  // annotation subquery / expression SQL
};

struct SelectItem {
  SelectKind kind = SelectKind::Column;
  ColumnRef col{};
  std::string out_alias;
  // Aggregate
  std::string agg_func;  // COUNT, SUM, AVG, MIN, MAX
  bool agg_distinct = false;
  bool agg_star = false;  // COUNT(*)
  // SqlFragment
  std::string sql_fragment;
  std::vector<std::uint32_t> fragment_param_idxs;
};

struct OrderItem {
  ColumnRef col{};
  bool desc = false;
  // Order by annotation alias
  std::string alias;
};

struct JoinEdge {
  JoinType type = JoinType::Inner;
  std::string alias;
  std::string table;
  std::string local_alias;
  std::string local_column;
  std::string remote_column;
};

struct Assignment {
  FieldId field = 0;
  std::uint32_t param_idx = 0;
  bool set_null = false;
};

enum class ResultMode : std::uint8_t {
  Model = 0,
  ValuesList,
  ValuesListFlat,
  ValuesDict,
  AggregateDict,  // .aggregate() → single row of named aggs
};

// Prefetch plan: executed after main query (ids known).
// Multi-hop lookups produce one PrefetchSpec per hop (in order).
struct PrefetchSpec {
  std::string lookup;  // full original path e.g. "books__tags"
  // Hop path relative to root of this prefetch chain segment.
  // Empty parent_path → parents are main-query instances.
  // Non-empty (e.g. "books") → parents are objects attached under that
  // cumulative path on the root instances.
  std::string parent_path;
  std::string hop;  // this hop's field name only, e.g. "tags"
  // Populated from schema at add_prefetch time for secondary query build.
  RelKind rel = RelKind::None;
  std::string remote_table;
  std::string remote_model_label;
  std::string remote_pk_column;
  std::string remote_fk_column;   // reverse FK column on remote
  std::string m2m_table;
  std::string m2m_column;         // through → parent
  std::string m2m_reverse_column; // through → remote
  std::string parent_pk_column;
  std::string cache_name;  // descriptor / related name for attach
};

// select_related column mapping for materialize
struct RelatedSelect {
  std::string path;          // "author" or "author__country"
  std::string join_alias;
  ModelId model_id = 0;
  std::vector<std::string> field_attnames;  // order of selected cols
  int select_offset = 0;     // index into result row where these cols start
  int select_count = 0;
};

struct Query {
  ModelId model = 0;
  DialectId dialect = DialectId::Postgres;
  StmtKind kind = StmtKind::Select;
  std::vector<SelectItem> select;
  std::vector<JoinEdge> joins;
  BoolExpr where{};
  bool has_where = false;
  BoolExpr having{};
  bool has_having = false;
  std::vector<OrderItem> order_by;
  // GROUP BY: column refs and/or annotation aliases
  std::vector<ColumnRef> group_by;
  std::vector<std::string> group_by_aliases;
  bool group_by_all_selected = false;
  std::optional<std::uint64_t> limit;
  std::uint64_t offset = 0;
  bool distinct = false;
  ResultMode result_mode = ResultMode::Model;
  std::vector<Assignment> assignments;
  std::vector<ParamValue> params;
  std::string base_alias;

  // select_related bookkeeping for from_db of related models
  std::vector<RelatedSelect> related_selects;
  // prefetch_related lookups (Python runs secondary queries)
  std::vector<PrefetchSpec> prefetches;

  // Concrete field attnames selected for base model (materialize)
  std::vector<std::string> base_attnames;
  int base_select_count = 0;

  std::uint32_t add_param(ParamValue v) {
    auto idx = static_cast<std::uint32_t>(params.size());
    params.push_back(std::move(v));
    return idx;
  }
};

[[nodiscard]] BoolExpr bool_atom(Pred p);
[[nodiscard]] BoolExpr bool_and(std::vector<BoolExpr> kids);
[[nodiscard]] BoolExpr bool_or(std::vector<BoolExpr> kids);
[[nodiscard]] BoolExpr bool_xor(std::vector<BoolExpr> kids);
[[nodiscard]] BoolExpr bool_not(BoolExpr child);
void bool_and_append(BoolExpr& dest, BoolExpr child);
void bool_or_append(BoolExpr& dest, BoolExpr child);

[[nodiscard]] std::optional<CmpOp> cmp_op_from_lookup(std::string_view lookup);

}  // namespace django::orm
