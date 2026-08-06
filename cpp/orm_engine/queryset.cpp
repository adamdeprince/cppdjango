#include "orm_engine/queryset.hpp"

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

QuerySet QuerySet::clone() const {
  QuerySet c = *this;
  return c;
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

bool QuerySet::filter_subquery(std::string_view field_name, CmpOp op,
                               std::string subquery_sql,
                               std::vector<ParamValue> subquery_params) {
  ColumnRef col;
  CmpOp parsed = op;
  bool isnull = false;
  if (!resolve_lookup_key(field_name, col, parsed, isnull)) {
    auto fid = resolve_field(field_name);
    if (!fid) {
      return false;
    }
    col.field = *fid;
  }
  Pred p;
  p.op = op;
  p.lhs = col;
  p.rhs_is_sql = true;
  // Ensure parentheses
  if (subquery_sql.empty() || subquery_sql.front() != '(') {
    p.rhs_sql = "(" + subquery_sql + ")";
  } else {
    p.rhs_sql = std::move(subquery_sql);
  }
  for (auto& v : subquery_params) {
    p.rhs_sql_param_idxs.push_back(query_.add_param(std::move(v)));
  }
  return append_pred(std::move(p));
}

std::string QuerySet::ensure_join(const ModelSchema& local_model,
                                  std::string_view local_alias,
                                  std::string_view fk_name,
                                  std::string_view path_prefix,
                                  JoinType join_type) {
  auto fit = local_model.field_by_name.find(std::string(fk_name));
  if (fit == local_model.field_by_name.end()) {
    return {};
  }
  const FieldSchema& rel = local_model.fields[fit->second];
  if (!rel.is_relation() || rel.remote_table.empty()) {
    return {};
  }
  std::string prefix(path_prefix);
  if (auto jt = join_aliases_.find(prefix); jt != join_aliases_.end()) {
    return jt->second;
  }

  const FieldSchema* local_pk = nullptr;
  if (local_model.pk_field < local_model.fields.size()) {
    local_pk = &local_model.fields[local_model.pk_field];
  }
  std::string local_pk_col =
      local_pk && !local_pk->column.empty() ? local_pk->column : "id";
  std::string remote_pk =
      rel.remote_pk_column.empty() ? std::string("id") : rel.remote_pk_column;

  if (rel.rel == RelKind::ForwardM2M || rel.rel == RelKind::ReverseM2M) {
    if (rel.m2m_table.empty()) {
      return {};
    }
    std::string through_alias = "T" + std::to_string(++join_counter_);
    std::string remote_alias = "J" + std::to_string(++join_counter_);
    JoinEdge through;
    through.type = join_type;
    through.alias = through_alias;
    through.table = rel.m2m_table;
    through.local_alias = std::string(local_alias);
    through.local_column = local_pk_col;
    through.remote_column =
        rel.m2m_column.empty() ? std::string("from_id") : rel.m2m_column;
    JoinEdge remote;
    remote.type = join_type;
    remote.alias = remote_alias;
    remote.table = rel.remote_table;
    remote.local_alias = through_alias;
    remote.local_column = rel.m2m_reverse_column.empty()
                              ? std::string("to_id")
                              : rel.m2m_reverse_column;
    remote.remote_column = remote_pk;
    query_.joins.push_back(std::move(through));
    query_.joins.push_back(std::move(remote));
    join_aliases_[prefix] = remote_alias;
    return remote_alias;
  }

  std::string alias = "J" + std::to_string(++join_counter_);
  JoinEdge edge;
  edge.type = join_type;
  edge.alias = alias;
  edge.table = rel.remote_table;
  edge.local_alias = std::string(local_alias);

  if (rel.rel == RelKind::ReverseFK) {
    edge.local_column = local_pk_col;
    edge.remote_column =
        rel.remote_fk_column.empty() ? std::string("id") : rel.remote_fk_column;
  } else {
    edge.local_column = rel.column;
    edge.remote_column = remote_pk;
  }
  query_.joins.push_back(std::move(edge));
  join_aliases_[prefix] = alias;
  return alias;
}

std::string QuerySet::ensure_path_joins(std::string_view path, JoinType join_type,
                                        const ModelSchema** out_model) {
  auto parts = split_lookup_sep(path);
  if (parts.empty()) {
    return {};
  }
  const SchemaRegistry& reg = SchemaRegistry::instance();
  const ModelSchema* current = reg.get(query_.model);
  if (!current) {
    return {};
  }
  std::string current_alias =
      query_.base_alias.empty() ? current->db_table : query_.base_alias;
  std::string path_prefix;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (!path_prefix.empty()) {
      path_prefix += "__";
    }
    path_prefix += parts[i];
    std::string alias =
        ensure_join(*current, current_alias, parts[i], path_prefix, join_type);
    if (alias.empty()) {
      return {};
    }
    auto fit = current->field_by_name.find(parts[i]);
    if (fit == current->field_by_name.end()) {
      return {};
    }
    const FieldSchema& fk = current->fields[fit->second];
    const ModelSchema* remote =
        fk.remote_model_label.empty()
            ? nullptr
            : reg.get_by_label(fk.remote_model_label);
    if (!remote) {
      return {};
    }
    current = remote;
    current_alias = alias;
  }
  if (out_model) {
    *out_model = current;
  }
  return current_alias;
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
    auto fit = current->field_by_name.find(path[i]);
    if (fit == current->field_by_name.end()) {
      return false;
    }
    const FieldSchema& fk = current->fields[fit->second];
    if (!fk.is_relation()) {
      return false;
    }
    const ModelSchema* remote = nullptr;
    if (!fk.remote_model_label.empty()) {
      remote = reg.get_by_label(fk.remote_model_label);
    }
    if (!remote) {
      if (i + 2 != path.size()) {
        return false;
      }
      col.field = 0;
      col.table_alias = alias;
      col.column_override = path.back();
      return true;
    }
    current = remote;
    current_alias = alias;
  }

  const std::string& target = path.back();
  auto tit = current->field_by_name.find(target);
  if (tit == current->field_by_name.end()) {
    col.field = 0;
    col.table_alias = current_alias;
    col.column_override = target;
    if (path.size() == 1) {
      return false;
    }
    return true;
  }
  const FieldSchema& tf = current->fields[tit->second];
  if (path.size() == 1) {
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

std::optional<Pred> QuerySet::pred_from_q_atom(const QNode& node) {
  if (node.has_rhs_sql) {
    ColumnRef col;
    CmpOp op = static_cast<CmpOp>(node.rhs_op);
    bool isnull = false;
    if (!resolve_lookup_key(node.key, col, op, isnull)) {
      auto fid = resolve_field(node.key);
      if (!fid) {
        return std::nullopt;
      }
      col.field = *fid;
    }
    Pred p;
    p.op = static_cast<CmpOp>(node.rhs_op);
    p.lhs = col;
    p.rhs_is_sql = true;
    p.rhs_sql = node.rhs_sql;
    if (p.rhs_sql.empty() || p.rhs_sql.front() != '(') {
      p.rhs_sql = "(" + node.rhs_sql + ")";
    }
    for (const auto& v : node.rhs_params) {
      p.rhs_sql_param_idxs.push_back(query_.add_param(v));
    }
    return p;
  }
  return pred_from_key_values(node.key, node.values);
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
  if (node.kind == 3) {
    auto pred = pred_from_q_atom(node);
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
  if (node.kind != 0 && node.kind != 1 && node.kind != 4) {
    return std::nullopt;
  }
  if (node.children.empty()) {
    BoolExpr e;
    if (node.kind == 4) {
      e.kind = BoolExpr::Kind::Xor;
    } else {
      e.kind = node.kind == 0 ? BoolExpr::Kind::And : BoolExpr::Kind::Or;
    }
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
  if (node.kind == 4) {
    return bool_xor(std::move(kids));
  }
  return node.kind == 0 ? bool_and(std::move(kids)) : bool_or(std::move(kids));
}

bool QuerySet::apply_q(const QNode& node) {
  auto expr = lower_q_node(node);
  if (!expr) {
    return false;
  }
  if (expr->kind == BoolExpr::Kind::And && expr->children.empty() &&
      node.kind == 0) {
    return true;
  }
  if (!query_.has_where) {
    query_.where = std::move(*expr);
    query_.has_where = true;
  } else {
    bool_and_append(query_.where, std::move(*expr));
  }
  return true;
}

bool QuerySet::values(const std::vector<std::string>& field_names, bool out_aliases,
                      ResultMode mode) {
  query_.select.clear();
  query_.related_selects.clear();
  query_.base_attnames.clear();
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
    query_.base_attnames.push_back(name);
  }
  query_.base_select_count = static_cast<int>(query_.select.size());
  return true;
}

void QuerySet::clear_select() {
  query_.select.clear();
  query_.base_attnames.clear();
  query_.related_selects.clear();
  query_.base_select_count = 0;
  query_.kind = StmtKind::Select;
}

bool QuerySet::select_model_columns() {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }
  clear_select();
  query_.result_mode = ResultMode::Model;
  for (const auto& f : m->fields) {
    if (f.column.empty()) {
      continue;
    }
    SelectItem it;
    it.col.field = f.id;
    query_.select.push_back(std::move(it));
    query_.base_attnames.push_back(f.attname.empty() ? f.name : f.attname);
  }
  query_.base_select_count = static_cast<int>(query_.select.size());
  return !query_.select.empty();
}

bool QuerySet::annotate_aggregate(std::string alias, std::string func,
                                  std::string field_name, bool distinct,
                                  bool star) {
  SelectItem it;
  it.kind = SelectKind::Aggregate;
  it.out_alias = std::move(alias);
  it.agg_func = std::move(func);
  it.agg_distinct = distinct;
  it.agg_star = star;
  if (!star) {
    ColumnRef col;
    CmpOp op = CmpOp::Eq;
    bool isnull = false;
    if (!resolve_lookup_key(field_name, col, op, isnull)) {
      auto fid = resolve_field(field_name);
      if (!fid) {
        return false;
      }
      col.field = *fid;
    }
    it.col = col;
  }
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::annotate_sql(std::string alias, std::string sql_fragment,
                            std::vector<ParamValue> params) {
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  if (sql_fragment.empty() || sql_fragment.front() != '(') {
    it.sql_fragment = "(" + sql_fragment + ")";
  } else {
    it.sql_fragment = std::move(sql_fragment);
  }
  for (auto& p : params) {
    it.fragment_param_idxs.push_back(query_.add_param(std::move(p)));
  }
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::group_by_fields(const std::vector<std::string>& field_names) {
  for (const auto& name : field_names) {
    ColumnRef col;
    CmpOp op = CmpOp::Eq;
    bool isnull = false;
    if (!resolve_lookup_key(name, col, op, isnull)) {
      auto fid = resolve_field(name);
      if (!fid) {
        return false;
      }
      col.field = *fid;
    }
    query_.group_by.push_back(col);
  }
  return true;
}

void QuerySet::group_by_selected_columns() { query_.group_by_all_selected = true; }

bool QuerySet::add_select_related(std::string_view path) {
  if (query_.select.empty()) {
    if (!select_model_columns()) {
      return false;
    }
  }
  const ModelSchema* remote = nullptr;
  std::string alias =
      ensure_path_joins(path, JoinType::LeftOuter, &remote);
  if (alias.empty() || !remote) {
    return false;
  }
  RelatedSelect rs;
  rs.path = std::string(path);
  rs.join_alias = alias;
  rs.model_id = remote->id;
  rs.select_offset = static_cast<int>(query_.select.size());
  for (const auto& f : remote->fields) {
    if (f.column.empty()) {
      continue;
    }
    SelectItem it;
    it.kind = SelectKind::Column;
    it.col.table_alias = alias;
    it.col.column_override = f.column;
    it.col.field = 0;
    query_.select.push_back(std::move(it));
    rs.field_attnames.push_back(f.attname.empty() ? f.name : f.attname);
  }
  rs.select_count = static_cast<int>(rs.field_attnames.size());
  query_.related_selects.push_back(std::move(rs));
  return true;
}

bool QuerySet::add_select_related_all(int max_depth) {
  if (max_depth < 1) {
    return false;
  }
  if (query_.select.empty()) {
    if (!select_model_columns()) {
      return false;
    }
  }
  const SchemaRegistry& reg = SchemaRegistry::instance();
  const ModelSchema* base = reg.get(query_.model);
  if (!base) {
    return false;
  }
  // BFS: (path, model, depth)
  struct Node {
    std::string path;
    ModelId model;
    int depth;
  };
  std::vector<Node> queue;
  queue.push_back({"", query_.model, 0});
  std::vector<std::string> paths;
  std::size_t qi = 0;
  while (qi < queue.size()) {
    Node cur = queue[qi++];
    if (cur.depth >= max_depth) {
      continue;
    }
    const ModelSchema* m = reg.get(cur.model);
    if (!m) {
      continue;
    }
    for (const auto& f : m->fields) {
      if (f.rel != RelKind::ForwardFK) {
        continue;  // O2O stored as FK; skip reverse/m2m for select_related
      }
      if (f.remote_model_label.empty()) {
        continue;
      }
      auto rid = reg.find_id(f.remote_model_label);
      if (!rid) {
        continue;
      }
      std::string path =
          cur.path.empty() ? f.name : (cur.path + "__" + f.name);
      paths.push_back(path);
      queue.push_back({path, *rid, cur.depth + 1});
    }
  }
  for (const auto& p : paths) {
    if (!add_select_related(p)) {
      return false;
    }
  }
  return true;
}

bool QuerySet::add_prefetch(std::string_view lookup) {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }
  // Single-hop prefetch for now (lookup may be "books" or "tags").
  auto parts = split_lookup_sep(lookup);
  if (parts.empty()) {
    return false;
  }
  // Use first hop field for relation metadata.
  auto fit = m->field_by_name.find(parts[0]);
  if (fit == m->field_by_name.end()) {
    return false;
  }
  const FieldSchema& f = m->fields[fit->second];
  if (!f.is_relation()) {
    return false;
  }
  PrefetchSpec p;
  p.lookup = std::string(lookup);
  p.rel = f.rel;
  p.remote_table = f.remote_table;
  p.remote_model_label = f.remote_model_label;
  p.remote_pk_column = f.remote_pk_column;
  p.remote_fk_column = f.remote_fk_column;
  p.m2m_table = f.m2m_table;
  p.m2m_column = f.m2m_column;
  p.m2m_reverse_column = f.m2m_reverse_column;
  p.cache_name = parts[0];
  if (m->pk_field < m->fields.size()) {
    p.parent_pk_column = m->fields[m->pk_field].column;
  }
  if (p.parent_pk_column.empty()) {
    p.parent_pk_column = "id";
  }
  query_.prefetches.push_back(std::move(p));
  return true;
}

bool QuerySet::filter_subquery_qs(std::string_view field_name, CmpOp op,
                                  const QuerySet& sub) {
  auto compiled = sub.compile();
  if (compiled.sql.empty()) {
    return false;
  }
  std::vector<ParamValue> flat;
  const auto& sp = sub.query().params;
  for (auto idx : compiled.param_order) {
    if (idx >= sp.size()) {
      return false;
    }
    flat.push_back(sp[idx]);
  }
  return filter_subquery(field_name, op, compiled.sql, std::move(flat));
}

bool QuerySet::annotate_subquery_qs(std::string alias, const QuerySet& sub) {
  auto compiled = sub.compile();
  if (compiled.sql.empty()) {
    return false;
  }
  std::vector<ParamValue> flat;
  const auto& sp = sub.query().params;
  for (auto idx : compiled.param_order) {
    if (idx >= sp.size()) {
      return false;
    }
    flat.push_back(sp[idx]);
  }
  return annotate_sql(std::move(alias), compiled.sql, std::move(flat));
}

bool QuerySet::annotate_case(
    std::string alias, const std::vector<std::pair<QNode, ParamValue>>& cases,
    bool has_else, ParamValue else_value) {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m || cases.empty()) {
    return false;
  }
  std::string frag = "CASE";
  std::vector<std::uint32_t> order;
  for (const auto& [when_node, then_val] : cases) {
    auto when_expr = lower_q_node(when_node);
    if (!when_expr) {
      return false;
    }
    frag += " WHEN ";
    append_bool_sql(frag, order, query_, *m, *when_expr);
    frag += " THEN %s";
    order.push_back(query_.add_param(then_val));
  }
  if (has_else) {
    frag += " ELSE %s";
    order.push_back(query_.add_param(std::move(else_value)));
  }
  frag += " END";
  // Convert order (param indices) into fragment_param_idxs — already absolute
  // indices into query_.params.
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  it.sql_fragment = std::move(frag);
  it.fragment_param_idxs = std::move(order);
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::annotate_binop(std::string alias, std::string lhs_field,
                              std::string op, ParamValue rhs) {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }
  ColumnRef col;
  CmpOp cop = CmpOp::Eq;
  bool isnull = false;
  if (!resolve_lookup_key(lhs_field, col, cop, isnull)) {
    auto fid = resolve_field(lhs_field);
    if (!fid) {
      return false;
    }
    col.field = *fid;
  }
  std::string frag;
  append_column_sql(frag, query_.dialect, query_, *m, col);
  frag += ' ';
  frag += op;
  frag += " %s";
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  it.sql_fragment = std::move(frag);
  it.fragment_param_idxs.push_back(query_.add_param(std::move(rhs)));
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::annotate_binop_fields(std::string alias, std::string lhs_field,
                                     std::string op, std::string rhs_field) {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }
  ColumnRef lcol, rcol;
  CmpOp cop = CmpOp::Eq;
  bool isnull = false;
  if (!resolve_lookup_key(lhs_field, lcol, cop, isnull)) {
    auto fid = resolve_field(lhs_field);
    if (!fid) {
      return false;
    }
    lcol.field = *fid;
  }
  if (!resolve_lookup_key(rhs_field, rcol, cop, isnull)) {
    auto fid = resolve_field(rhs_field);
    if (!fid) {
      return false;
    }
    rcol.field = *fid;
  }
  std::string frag;
  append_column_sql(frag, query_.dialect, query_, *m, lcol);
  frag += ' ';
  frag += op;
  frag += ' ';
  append_column_sql(frag, query_.dialect, query_, *m, rcol);
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  it.sql_fragment = std::move(frag);
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::annotate_value(std::string alias, ParamValue value) {
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  it.sql_fragment = "%s";
  it.fragment_param_idxs.push_back(query_.add_param(std::move(value)));
  query_.select.push_back(std::move(it));
  return true;
}

bool QuerySet::annotate_f(std::string alias, std::string field_name) {
  const ModelSchema* m = SchemaRegistry::instance().get(query_.model);
  if (!m) {
    return false;
  }
  ColumnRef col;
  CmpOp cop = CmpOp::Eq;
  bool isnull = false;
  if (!resolve_lookup_key(field_name, col, cop, isnull)) {
    auto fid = resolve_field(field_name);
    if (!fid) {
      return false;
    }
    col.field = *fid;
  }
  std::string frag;
  append_column_sql(frag, query_.dialect, query_, *m, col);
  SelectItem it;
  it.kind = SelectKind::SqlFragment;
  it.out_alias = std::move(alias);
  it.sql_fragment = std::move(frag);
  query_.select.push_back(std::move(it));
  return true;
}

QuerySet::PrefetchSql QuerySet::compile_prefetch_secondary(
    const PrefetchSpec& spec,
    const std::vector<ParamValue>& parent_pks) const {
  PrefetchSql out;
  if (parent_pks.empty() || spec.remote_model_label.empty()) {
    return out;
  }
  const SchemaRegistry& reg = SchemaRegistry::instance();
  auto rid = reg.find_id(spec.remote_model_label);
  if (!rid) {
    return out;
  }
  QuerySet sub(*rid, query_.dialect);
  if (!sub.select_model_columns()) {
    return out;
  }
  // Filter related rows by parent link.
  if (spec.rel == RelKind::ReverseFK) {
    // remote.fk IN parent_pks
    const std::string& fkcol = spec.remote_fk_column.empty()
                                   ? std::string("id")
                                   : spec.remote_fk_column;
    Pred p;
    p.op = CmpOp::In;
    p.lhs.field = 0;
    p.lhs.table_alias = sub.query().base_alias;
    p.lhs.column_override = fkcol;
    for (const auto& v : parent_pks) {
      p.param_idxs.push_back(sub.query_mut().add_param(v));
    }
    sub.query_mut().where = bool_atom(std::move(p));
    sub.query_mut().has_where = true;
  } else if (spec.rel == RelKind::ForwardM2M || spec.rel == RelKind::ReverseM2M) {
    // JOIN through WHERE through.parent_col IN pks
    if (spec.m2m_table.empty()) {
      return out;
    }
    JoinEdge through;
    through.type = JoinType::Inner;
    through.alias = "PF";
    through.table = spec.m2m_table;
    through.local_alias = sub.query().base_alias;
    // remote side of through links to remote pk
    through.local_column =
        reg.get(*rid) && reg.get(*rid)->pk_field < reg.get(*rid)->fields.size()
            ? reg.get(*rid)->fields[reg.get(*rid)->pk_field].column
            : "id";
    through.remote_column = spec.m2m_reverse_column.empty()
                                ? std::string("to_id")
                                : spec.m2m_reverse_column;
    // Actually ON remote.pk = through.m2m_reverse AND through.m2m_column IN (...)
    // Rewrite: JOIN through ON through.m2m_reverse = remote.pk
    through.local_column =
        reg.get(*rid)->fields[reg.get(*rid)->pk_field].column.empty()
            ? "id"
            : reg.get(*rid)->fields[reg.get(*rid)->pk_field].column;
    // emit: local_alias.local_column = alias.remote_column
    // so remote.pk = through.m2m_reverse
    // local_alias = remote table alias, local_column = pk
    // alias = through, remote_column = m2m_reverse
    JoinEdge j;
    j.type = JoinType::Inner;
    j.alias = "PF";
    j.table = spec.m2m_table;
    j.local_alias = sub.query().base_alias;
    j.local_column = through.local_column;
    j.remote_column = spec.m2m_reverse_column.empty() ? "to_id"
                                                     : spec.m2m_reverse_column;
    sub.query_mut().joins.push_back(j);
    Pred p;
    p.op = CmpOp::In;
    p.lhs.field = 0;
    p.lhs.table_alias = "PF";
    p.lhs.column_override =
        spec.m2m_column.empty() ? std::string("from_id") : spec.m2m_column;
    for (const auto& v : parent_pks) {
      p.param_idxs.push_back(sub.query_mut().add_param(v));
    }
    sub.query_mut().where = bool_atom(std::move(p));
    sub.query_mut().has_where = true;
  } else if (spec.rel == RelKind::ForwardFK) {
    // parent_pks are FK values; match remote PK IN (...)
    Pred p;
    p.op = CmpOp::In;
    p.lhs.field = 0;
    p.lhs.table_alias = sub.query().base_alias;
    p.lhs.column_override =
        spec.remote_pk_column.empty() ? std::string("id") : spec.remote_pk_column;
    for (const auto& v : parent_pks) {
      p.param_idxs.push_back(sub.query_mut().add_param(v));
    }
    sub.query_mut().where = bool_atom(std::move(p));
    sub.query_mut().has_where = true;
  } else {
    return out;
  }
  auto compiled = sub.compile();
  out.sql = std::move(compiled.sql);
  const auto& sp = sub.query().params;
  for (auto idx : compiled.param_order) {
    if (idx < sp.size()) {
      out.params.push_back(sp[idx]);
    }
  }
  return out;
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
    ColumnRef col;
    CmpOp op = CmpOp::Eq;
    bool isnull = false;
    if (!resolve_lookup_key(field_name, col, op, isnull)) {
      return false;
    }
    OrderItem o;
    o.col = col;
    o.desc = desc;
    query_.order_by.push_back(o);
    return true;
  }
  OrderItem o;
  o.col.field = *fid;
  o.desc = desc;
  query_.order_by.push_back(o);
  return true;
}

bool QuerySet::order_by_alias(std::string_view alias, bool desc) {
  OrderItem o;
  o.alias = std::string(alias);
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
void QuerySet::set_distinct(bool v) { query_.distinct = v; }

CompiledSql QuerySet::compile(const SchemaRegistry& reg) const {
  return compile_query(query_, reg);
}

}  // namespace django::orm
