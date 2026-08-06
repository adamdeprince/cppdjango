#include "orm_engine/query.hpp"

namespace django::orm {

BoolExpr bool_atom(Pred p) {
  BoolExpr e;
  e.kind = BoolExpr::Kind::Atom;
  e.atom = std::move(p);
  return e;
}

BoolExpr bool_and(std::vector<BoolExpr> kids) {
  BoolExpr e;
  e.kind = BoolExpr::Kind::And;
  e.children = std::move(kids);
  return e;
}

BoolExpr bool_or(std::vector<BoolExpr> kids) {
  BoolExpr e;
  e.kind = BoolExpr::Kind::Or;
  e.children = std::move(kids);
  return e;
}

BoolExpr bool_xor(std::vector<BoolExpr> kids) {
  BoolExpr e;
  e.kind = BoolExpr::Kind::Xor;
  e.children = std::move(kids);
  return e;
}

BoolExpr bool_not(BoolExpr child) {
  BoolExpr e;
  e.kind = BoolExpr::Kind::Not;
  e.children.push_back(std::move(child));
  return e;
}

void bool_and_append(BoolExpr& dest, BoolExpr child) {
  if (dest.kind == BoolExpr::Kind::And) {
    dest.children.push_back(std::move(child));
    return;
  }
  BoolExpr old = std::move(dest);
  dest = bool_and({});
  dest.children.push_back(std::move(old));
  dest.children.push_back(std::move(child));
}

void bool_or_append(BoolExpr& dest, BoolExpr child) {
  if (dest.kind == BoolExpr::Kind::Or) {
    dest.children.push_back(std::move(child));
    return;
  }
  BoolExpr old = std::move(dest);
  dest = bool_or({});
  dest.children.push_back(std::move(old));
  dest.children.push_back(std::move(child));
}

std::optional<CmpOp> cmp_op_from_lookup(std::string_view lookup) {
  if (lookup.empty() || lookup == "exact") {
    return CmpOp::Eq;
  }
  if (lookup == "iexact") {
    return CmpOp::Eq;  // emit same; collations later
  }
  if (lookup == "gt") {
    return CmpOp::Gt;
  }
  if (lookup == "gte") {
    return CmpOp::Gte;
  }
  if (lookup == "lt") {
    return CmpOp::Lt;
  }
  if (lookup == "lte") {
    return CmpOp::Lte;
  }
  if (lookup == "in") {
    return CmpOp::In;
  }
  if (lookup == "isnull") {
    return CmpOp::IsNull;
  }
  if (lookup == "ne" || lookup == "neq") {
    return CmpOp::Ne;
  }
  return std::nullopt;
}

}  // namespace django::orm
