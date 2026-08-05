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
  // Promote existing tree + new child into And.
  BoolExpr old = std::move(dest);
  dest = bool_and({});
  dest.children.push_back(std::move(old));
  dest.children.push_back(std::move(child));
}

}  // namespace django::orm
