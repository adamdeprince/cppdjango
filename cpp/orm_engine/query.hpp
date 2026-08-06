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

// Bound parameter stored in the query's arena (typed, no Python).
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
  // empty → base table (model db_table)
  std::string table_alias;
  // If set, emit this column name instead of schema field column (join paths).
  std::string column_override;
};

struct Pred {
  CmpOp op = CmpOp::Eq;
  ColumnRef lhs{};
  std::vector<std::uint32_t> param_idxs;
  bool is_null_negated = false;
};

struct BoolExpr {
  enum class Kind : std::uint8_t { Atom = 0, And, Or, Not, Xor };
  Kind kind = Kind::Atom;
  Pred atom{};
  std::vector<BoolExpr> children;
};

struct SelectItem {
  ColumnRef col{};
  std::string out_alias;
};

struct OrderItem {
  ColumnRef col{};
  bool desc = false;
};

struct JoinEdge {
  JoinType type = JoinType::Inner;
  std::string alias;          // joined table alias
  std::string table;          // physical table name
  std::string local_alias;    // left side alias
  std::string local_column;   // FK column on left
  std::string remote_column;  // PK column on right
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
};

struct Query {
  ModelId model = 0;
  DialectId dialect = DialectId::Postgres;
  StmtKind kind = StmtKind::Select;
  std::vector<SelectItem> select;
  std::vector<JoinEdge> joins;
  BoolExpr where{};
  bool has_where = false;
  std::vector<OrderItem> order_by;
  std::optional<std::uint64_t> limit;
  std::uint64_t offset = 0;
  bool distinct = false;
  ResultMode result_mode = ResultMode::Model;
  std::vector<Assignment> assignments;  // UPDATE SET
  std::vector<ParamValue> params;
  // alias → table name for resolve (base alias = db_table)
  std::string base_alias;

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

// Parse "exact" / "gt" / "in" / "isnull" / …
[[nodiscard]] std::optional<CmpOp> cmp_op_from_lookup(std::string_view lookup);

}  // namespace django::orm
