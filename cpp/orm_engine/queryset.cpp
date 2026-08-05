#include "orm_engine/queryset.hpp"

#include <sstream>

namespace django::orm {

QuerySet::QuerySet(ModelId model, DialectId dialect) {
  query_.model = model;
  query_.dialect = dialect;
  const ModelSchema* m = SchemaRegistry::instance().get(model);
  if (m) {
    query_.base_alias = m->db_table;
  }
}

std::optional<FieldId> QuerySet::resolve_field(std::string_view name) const {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return std::nullopt;
  }
  auto it = m->field_by_name.find(std::string(name));
  if (it == m->field_by_name.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool QuerySet::append_pred(Pred p) {
  BoolExpr atom = bool_atom(std::move(p));
  if (!query_.has_where) {
    query_.where = std::move(atom);
    query_.has_where = true;
  } else {
    bool_and_append(query_.where, std::move(atom));
  }
  return true;
}

bool QuerySet::filter_eq(std::string_view field_name, ParamValue value) {
  return filter_cmp(field_name, CmpOp::Eq, std::move(value));
}

bool QuerySet::filter_cmp(std::string_view field_name, CmpOp op, ParamValue value) {
  ColumnRef col;
  // Prefer plain field + explicit op (do not let lookup parser force exact).
  if (auto fid = resolve_field(field_name)) {
    col.field = *fid;
  } else {
    CmpOp parsed = op;
    bool isnull = false;
    if (!resolve_lookup_key(field_name, col, parsed, isnull)) {
      return false;
    }
    op = parsed;
    if (isnull || op == CmpOp::IsNull) {
      Pred p;
      p.op = CmpOp::IsNull;
      p.lhs = col;
      p.is_null_negated =
          (value.kind == ParamValue::Kind::Bool) ? !value.b : false;
      return append_pred(std::move(p));
    }
  }
  if (op == CmpOp::IsNull) {
    Pred p;
    p.op = CmpOp::IsNull;
    p.lhs = col;
    p.is_null_negated =
        (value.kind == ParamValue::Kind::Bool) ? !value.b : false;
    return append_pred(std::move(p));
  }
  Pred p;
  p.op = op;
  p.lhs = col;
  p.param_idxs.push_back(query_.add_param(std::move(value)));
  return append_pred(std::move(p));
}

bool QuerySet::filter_isnull(std::string_view field_name, bool is_null) {
  auto fid = resolve_field(field_name);
  if (!fid) {
    ColumnRef col;
    CmpOp op = CmpOp::IsNull;
    bool dummy = false;
    if (!resolve_lookup_key(std::string(field_name) + "__isnull", col, op, dummy)) {
      return false;
    }
    Pred p;
    p.op = CmpOp::IsNull;
    p.lhs = col;
    p.is_null_negated = !is_null;
    return append_pred(std::move(p));
  }
  Pred p;
  p.op = CmpOp::IsNull;
  p.lhs.field = *fid;
  p.is_null_negated = !is_null;
  return append_pred(std::move(p));
}

bool QuerySet::filter_in(std::string_view field_name,
                         std::vector<ParamValue> values) {
  if (values.empty()) {
    return false;
  }
  ColumnRef col;
  CmpOp op = CmpOp::In;
  bool isnull = false;
  if (!resolve_lookup_key(field_name, col, op, isnull)) {
    auto fid = resolve_field(field_name);
    if (!fid) {
      return false;
    }
    col.field = *fid;
  }
  Pred p;
  p.op = CmpOp::In;
  p.lhs = col;
  for (auto& v : values) {
    p.param_idxs.push_back(query_.add_param(std::move(v)));
  }
  return append_pred(std::move(p));
}

bool QuerySet::resolve_lookup_key(std::string_view key, ColumnRef& col, CmpOp& op,
                                  bool& is_isnull_lookup) {
  // Split on __
  std::vector<std::string> parts;
  std::string cur;
  for (char c : key) {
    if (c == '_' && !cur.empty() && cur.back() == '_') {
      // shouldn't happen single pass — handle "__"
    }
  }
  // Manual split on "__"
  std::string s(key);
  std::size_t start = 0;
  while (start <= s.size()) {
    auto pos = s.find("__", start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 2;
  }
  if (parts.empty() || parts[0].empty()) {
    return false;
  }

  is_isnull_lookup = false;
  op = CmpOp::Eq;
  // Last part may be lookup
  std::string lookup = "exact";
  std::vector<std::string> path = parts;
  if (path.size() >= 2) {
    auto maybe = cmp_op_from_lookup(path.back());
    if (maybe) {
      lookup = path.back();
      op = *maybe;
      path.pop_back();
      if (lookup == "isnull") {
        is_isnull_lookup = true;
      }
    }
  }
  if (path.empty()) {
    return false;
  }

  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }

  // Single segment field on base model
  if (path.size() == 1) {
    auto it = m->field_by_name.find(path[0]);
    if (it == m->field_by_name.end()) {
      return false;
    }
    col.field = it->second;
    col.table_alias.clear();
    col.column_override.clear();
    return true;
  }

  // FK path: one hop only in v1 (fk__field)
  if (path.size() != 2) {
    return false;  // deeper joins later
  }
  auto fit = m->field_by_name.find(path[0]);
  if (fit == m->field_by_name.end()) {
    return false;
  }
  const FieldSchema& fk = m->fields[fit->second];
  if (!fk.is_relation || fk.remote_table.empty()) {
    return false;
  }

  // Ensure join
  std::string path_key = path[0];
  std::string alias;
  if (auto jt = join_aliases_.find(path_key); jt != join_aliases_.end()) {
    alias = jt->second;
  } else {
    alias = "J" + std::to_string(++join_counter_);
    JoinEdge edge;
    edge.type = JoinType::Inner;
    edge.alias = alias;
    edge.table = fk.remote_table;
    edge.local_alias = query_.base_alias.empty() ? m->db_table : query_.base_alias;
    edge.local_column = fk.column;
    edge.remote_column =
        fk.remote_pk_column.empty() ? std::string("id") : fk.remote_pk_column;
    query_.joins.push_back(edge);
    join_aliases_[path_key] = alias;
  }

  // Target column on remote: use column_override = path[1] as name (assume
  // same as column for simple schemas). For attname mapping we'd need remote
  // schema; use path[1] as column name.
  col.field = 0;
  col.table_alias = alias;
  col.column_override = path[1];
  return true;
}

bool QuerySet::filter_kwargs(
    const std::vector<std::pair<std::string, std::vector<ParamValue>>>& items,
    bool disjunctive) {
  if (items.empty()) {
    return true;
  }
  std::vector<BoolExpr> atoms;
  for (const auto& [key, values] : items) {
    ColumnRef col;
    CmpOp op = CmpOp::Eq;
    bool isnull = false;
    if (!resolve_lookup_key(key, col, op, isnull)) {
      return false;
    }
    Pred p;
    p.lhs = col;
    if (isnull || op == CmpOp::IsNull) {
      p.op = CmpOp::IsNull;
      bool is_null = true;
      if (!values.empty() && values[0].kind == ParamValue::Kind::Bool) {
        is_null = values[0].b;
      }
      p.is_null_negated = !is_null;
    } else if (op == CmpOp::In) {
      if (values.empty()) {
        return false;
      }
      p.op = CmpOp::In;
      for (const auto& v : values) {
        p.param_idxs.push_back(query_.add_param(v));
      }
    } else {
      if (values.size() != 1) {
        return false;
      }
      p.op = op;
      p.param_idxs.push_back(query_.add_param(values[0]));
    }
    atoms.push_back(bool_atom(std::move(p)));
  }

  BoolExpr combined =
      disjunctive ? bool_or(std::move(atoms)) : bool_and(std::move(atoms));
  if (!query_.has_where) {
    query_.where = std::move(combined);
    query_.has_where = true;
  } else if (disjunctive) {
    bool_or_append(query_.where, std::move(combined));
  } else {
    bool_and_append(query_.where, std::move(combined));
  }
  return true;
}

bool QuerySet::values(const std::vector<std::string>& field_names, bool out_aliases,
                      ResultMode mode) {
  query_.select.clear();
  query_.result_mode = mode;
  query_.kind = StmtKind::Select;
  for (const auto& name : field_names) {
    auto fid = resolve_field(name);
    if (!fid) {
      query_.select.clear();
      return false;
    }
    SelectItem it;
    it.col.field = *fid;
    if (out_aliases) {
      it.out_alias = name;
    }
    query_.select.push_back(std::move(it));
  }
  return true;
}

bool QuerySet::add_update(std::string_view field_name, ParamValue value) {
  auto fid = resolve_field(field_name);
  if (!fid) {
    return false;
  }
  query_.kind = StmtKind::Update;
  Assignment a;
  a.field = *fid;
  a.param_idx = query_.add_param(std::move(value));
  query_.assignments.push_back(a);
  return true;
}

bool QuerySet::add_update_null(std::string_view field_name) {
  auto fid = resolve_field(field_name);
  if (!fid) {
    return false;
  }
  query_.kind = StmtKind::Update;
  Assignment a;
  a.field = *fid;
  a.set_null = true;
  query_.assignments.push_back(a);
  return true;
}

void QuerySet::set_delete() { query_.kind = StmtKind::Delete; }

bool QuerySet::order_by(std::string_view field_name, bool desc) {
  auto fid = resolve_field(field_name);
  if (!fid) {
    return false;
  }
  OrderItem o;
  o.col.field = *fid;
  o.desc = desc;
  query_.order_by.push_back(o);
  return true;
}

void QuerySet::set_limit(std::uint64_t n) { query_.limit = n; }
void QuerySet::set_offset(std::uint64_t n) { query_.offset = n; }
void QuerySet::clear_limits() {
  query_.limit = std::nullopt;
  query_.offset = 0;
}

CompiledSql QuerySet::compile(const SchemaRegistry& reg) const {
  return compile_query(query_, reg);
}

}  // namespace django::orm
