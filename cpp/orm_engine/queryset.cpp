#include "orm_engine/queryset.hpp"

namespace django::orm {

QuerySet::QuerySet(ModelId model, DialectId dialect) {
  query_.model = model;
  query_.dialect = dialect;
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

bool QuerySet::filter_eq(std::string_view field_name, ParamValue value) {
  return filter_cmp(field_name, CmpOp::Eq, std::move(value));
}

bool QuerySet::filter_cmp(std::string_view field_name, CmpOp op, ParamValue value) {
  auto fid = resolve_field(field_name);
  if (!fid) {
    return false;
  }
  Pred p;
  p.op = op;
  p.lhs.field = *fid;
  p.param_idxs.push_back(query_.add_param(std::move(value)));
  BoolExpr atom = bool_atom(std::move(p));
  if (!query_.has_where) {
    query_.where = std::move(atom);
    query_.has_where = true;
  } else {
    bool_and_append(query_.where, std::move(atom));
  }
  return true;
}

bool QuerySet::filter_in(std::string_view field_name,
                         std::vector<ParamValue> values) {
  auto fid = resolve_field(field_name);
  if (!fid || values.empty()) {
    return false;
  }
  Pred p;
  p.op = CmpOp::In;
  p.lhs.field = *fid;
  for (auto& v : values) {
    p.param_idxs.push_back(query_.add_param(std::move(v)));
  }
  BoolExpr atom = bool_atom(std::move(p));
  if (!query_.has_where) {
    query_.where = std::move(atom);
    query_.has_where = true;
  } else {
    bool_and_append(query_.where, std::move(atom));
  }
  return true;
}

bool QuerySet::values(const std::vector<std::string>& field_names, bool out_aliases,
                      ResultMode mode) {
  query_.select.clear();
  query_.result_mode = mode;
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

void QuerySet::set_limit(std::uint64_t n) { query_.limit = n; }
void QuerySet::set_offset(std::uint64_t n) { query_.offset = n; }
void QuerySet::clear_limits() {
  query_.limit = std::nullopt;
  query_.offset = 0;
}

CompiledSql QuerySet::compile(const SchemaRegistry& reg) const {
  return compile_select(query_, reg);
}

}  // namespace django::orm
