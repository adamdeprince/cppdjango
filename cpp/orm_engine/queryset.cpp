#include "orm_engine/queryset.hpp"

#include <sstream>

namespace django::orm {
namespace {

std::vector<std::string> split_lookup_sep(std::string_view key) {
  std::vector<std::string> parts;
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
  return parts;
}

}  // namespace

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

std::string QuerySet::ensure_join(const ModelSchema& local_model,
                                  std::string_view local_alias,
                                  std::string_view fk_name,
                                  std::string_view path_prefix) {
  auto fit = local_model.field_by_name.find(std::string(fk_name));
  if (fit == local_model.field_by_name.end()) {
    return {};
  }
  const FieldSchema& fk = local_model.fields[fit->second];
  if (!fk.is_relation || fk.remote_table.empty()) {
    return {};
  }
  std::string prefix(path_prefix);
  if (auto jt = join_aliases_.find(prefix); jt != join_aliases_.end()) {
    return jt->second;
  }
  std::string alias = "J" + std::to_string(++join_counter_);
  JoinEdge edge;
  edge.type = JoinType::Inner;
  edge.alias = alias;
  edge.table = fk.remote_table;
  edge.local_alias = std::string(local_alias);
  edge.local_column = fk.column;
  edge.remote_column =
      fk.remote_pk_column.empty() ? std::string("id") : fk.remote_pk_column;
  query_.joins.push_back(std::move(edge));
  join_aliases_[prefix] = alias;
  return alias;
}

bool QuerySet::resolve_lookup_key(std::string_view key, ColumnRef& col, CmpOp& op,
                                  bool& is_isnull_lookup) {
  auto parts = split_lookup_sep(key);
  if (parts.empty() || parts[0].empty()) {
    return false;
  }

  is_isnull_lookup = false;
  op = CmpOp::Eq;
  std::vector<std::string> path = parts;
  if (path.size() >= 2) {
    auto maybe = cmp_op_from_lookup(path.back());
    if (maybe) {
      op = *maybe;
      if (path.back() == "isnull") {
        is_isnull_lookup = true;
      }
      path.pop_back();
    }
  }
  if (path.empty()) {
    return false;
  }

  const SchemaRegistry& reg = SchemaRegistry::instance();
  const ModelSchema* current = reg.get(query_.model);
  if (!current) {
    return false;
  }
  std::string current_alias =
      query_.base_alias.empty() ? current->db_table : query_.base_alias;
  std::string path_prefix;

  // Walk relation hops: path[0]..path[n-2]
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    if (!path_prefix.empty()) {
      path_prefix += "__";
    }
    path_prefix += path[i];
    std::string alias =
        ensure_join(*current, current_alias, path[i], path_prefix);
    if (alias.empty()) {
      return false;
    }
    // Advance to remote model schema when registered.
    auto fit = current->field_by_name.find(path[i]);
    if (fit == current->field_by_name.end()) {
      return false;
    }
    const FieldSchema& fk = current->fields[fit->second];
    const ModelSchema* remote = nullptr;
    if (!fk.remote_model_label.empty()) {
      remote = reg.get_by_label(fk.remote_model_label);
    }
    if (!remote) {
      // Last hop only: allow column_override without full remote schema.
      if (i + 2 != path.size()) {
        return false;  // need intermediate schema for multi-hop
      }
      col.field = 0;
      col.table_alias = alias;
      col.column_override = path.back();
      return true;
    }
    current = remote;
    current_alias = alias;
  }

  // Target field on `current` model at `current_alias`
  const std::string& target = path.back();
  auto tit = current->field_by_name.find(target);
  if (tit == current->field_by_name.end()) {
    // Fall back to treating target as raw column name on current alias.
    col.field = 0;
    col.table_alias = current_alias;
    col.column_override = target;
    // Only valid if we joined at least once (not base unresolved field).
    if (path.size() == 1) {
      return false;
    }
    return true;
  }
  const FieldSchema& tf = current->fields[tit->second];
  if (path.size() == 1) {
    // Base model field — use field id so compile uses schema column.
    col.field = tf.id;
    col.table_alias.clear();
    col.column_override.clear();
    return true;
  }
  col.field = 0;
  col.table_alias = current_alias;
  col.column_override = tf.column.empty() ? target : tf.column;
  return true;
}

std::optional<Pred> QuerySet::pred_from_key_values(
    std::string_view key, const std::vector<ParamValue>& values) {
  ColumnRef col;
  CmpOp op = CmpOp::Eq;
  bool isnull = false;
  if (!resolve_lookup_key(key, col, op, isnull)) {
    return std::nullopt;
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
    return p;
  }
  if (op == CmpOp::In) {
    if (values.empty()) {
      return std::nullopt;
    }
    p.op = CmpOp::In;
    for (const auto& v : values) {
      p.param_idxs.push_back(query_.add_param(v));
    }
    return p;
  }
  if (values.size() != 1) {
    return std::nullopt;
  }
  p.op = op;
  p.param_idxs.push_back(query_.add_param(values[0]));
  return p;
}

bool QuerySet::filter_kwargs(
    const std::vector<std::pair<std::string, std::vector<ParamValue>>>& items,
    bool disjunctive) {
  if (items.empty()) {
    return true;
  }
  std::vector<BoolExpr> atoms;
  for (const auto& [key, values] : items) {
    auto pred = pred_from_key_values(key, values);
    if (!pred) {
      return false;
    }
    atoms.push_back(bool_atom(std::move(*pred)));
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

std::optional<BoolExpr> QuerySet::lower_q_node(const QNode& node) {
  // 0=And, 1=Or, 2=Not, 3=Atom
  if (node.kind == 3) {
    auto pred = pred_from_key_values(node.key, node.values);
    if (!pred) {
      return std::nullopt;
    }
    return bool_atom(std::move(*pred));
  }
  if (node.kind == 2) {
    if (node.children.size() != 1) {
      return std::nullopt;
    }
    auto child = lower_q_node(node.children[0]);
    if (!child) {
      return std::nullopt;
    }
    return bool_not(std::move(*child));
  }
  if (node.kind != 0 && node.kind != 1) {
    return std::nullopt;
  }
  if (node.children.empty()) {
    // Empty AND → true (no filter); empty OR → false — match Django roughly
    // by emitting a no-op atom for AND and 0=1 style via empty Or children
    // compile already maps empty And to 1=1 and empty Or to 0=1.
    BoolExpr e;
    e.kind = node.kind == 0 ? BoolExpr::Kind::And : BoolExpr::Kind::Or;
    return e;
  }
  std::vector<BoolExpr> kids;
  kids.reserve(node.children.size());
  for (const auto& c : node.children) {
    auto lc = lower_q_node(c);
    if (!lc) {
      return std::nullopt;
    }
    kids.push_back(std::move(*lc));
  }
  return node.kind == 0 ? bool_and(std::move(kids)) : bool_or(std::move(kids));
}

bool QuerySet::apply_q(const QNode& node) {
  auto expr = lower_q_node(node);
  if (!expr) {
    return false;
  }
  // Empty And with no children: no-op success
  if (expr->kind == BoolExpr::Kind::And && expr->children.empty() &&
      node.kind == 0) {
    return true;
  }
  if (!query_.has_where) {
    query_.where = std::move(*expr);
    query_.has_where = true;
  } else {
    // Q from filter() ANDs with existing where
    bool_and_append(query_.where, std::move(*expr));
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
