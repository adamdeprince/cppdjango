// Native query graph (authoritative IR for the data plane).
#pragma once

#include "orm_engine/schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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
  // empty → use model db_table as alias (single-table)
  std::string table_alias;
};

struct Pred {
  CmpOp op = CmpOp::Eq;
  ColumnRef lhs{};
  // Eq/Ne/Lt/...: single index into Query::params
  // In: list of indices
  // IsNull: negated flag in is_null_negated; no params
  std::vector<std::uint32_t> param_idxs;
  bool is_null_negated = false;
};

struct BoolExpr {
  enum class Kind : std::uint8_t { Atom = 0, And, Or, Not };
  Kind kind = Kind::Atom;
  Pred atom{};
  std::vector<BoolExpr> children;  // And/Or/Not
};

struct SelectItem {
  ColumnRef col{};
  std::string out_alias;  // empty → no AS
};

struct OrderItem {
  ColumnRef col{};
  bool desc = false;
};

enum class ResultMode : std::uint8_t {
  Model = 0,
  ValuesList,
  ValuesListFlat,
  ValuesDict,
};

// Single-table query graph (v1). Extensible toward full IR.
struct Query {
  ModelId model = 0;
  DialectId dialect = DialectId::Postgres;
  std::vector<SelectItem> select;  // empty → all concrete columns
  BoolExpr where{};                // Kind::And with empty children = no WHERE
  bool has_where = false;
  std::vector<OrderItem> order_by;
  std::optional<std::uint64_t> limit;
  std::uint64_t offset = 0;
  bool distinct = false;
  ResultMode result_mode = ResultMode::Model;
  std::vector<ParamValue> params;

  std::uint32_t add_param(ParamValue v) {
    auto idx = static_cast<std::uint32_t>(params.size());
    params.push_back(std::move(v));
    return idx;
  }
};

// Helpers to build boolean trees.
[[nodiscard]] BoolExpr bool_atom(Pred p);
[[nodiscard]] BoolExpr bool_and(std::vector<BoolExpr> kids);
[[nodiscard]] BoolExpr bool_or(std::vector<BoolExpr> kids);
[[nodiscard]] BoolExpr bool_not(BoolExpr child);
void bool_and_append(BoolExpr& dest, BoolExpr child);

}  // namespace django::orm
