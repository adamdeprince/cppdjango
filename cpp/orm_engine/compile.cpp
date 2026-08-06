#include "orm_engine/compile.hpp"

#include <stdexcept>

namespace django::orm {
namespace {

void append_quoted(std::string& out, DialectId d, std::string_view name) {
  out += quote_ident(d, name);
}

const FieldSchema* field_of(const ModelSchema& m, FieldId id) {
  if (id >= m.fields.size()) {
    return nullptr;
  }
  return &m.fields[id];
}

std::string resolve_alias(const Query& q, const ModelSchema& m,
                          const ColumnRef& c) {
  if (!c.table_alias.empty()) {
    return c.table_alias;
  }
  if (!q.base_alias.empty()) {
    return q.base_alias;
  }
  return m.db_table;
}

void emit_column(std::string& out, DialectId d, const Query& q,
                 const ModelSchema& m, const ColumnRef& c) {
  append_quoted(out, d, resolve_alias(q, m, c));
  out += '.';
  if (!c.column_override.empty()) {
    append_quoted(out, d, c.column_override);
    return;
  }
  auto* f = field_of(m, c.field);
  if (!f) {
    throw std::runtime_error("orm compile: bad field id");
  }
  append_quoted(out, d, f->column);
}

void emit_pred(std::string& out, std::vector<std::uint32_t>& order, DialectId d,
               const Query& q, const ModelSchema& m, const Pred& p) {
  emit_column(out, d, q, m, p.lhs);
  switch (p.op) {
    case CmpOp::Eq:
      out += " = %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::Ne:
      out += " <> %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::Lt:
      out += " < %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::Lte:
      out += " <= %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::Gt:
      out += " > %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::Gte:
      out += " >= %s";
      order.push_back(p.param_idxs.at(0));
      break;
    case CmpOp::In: {
      out += " IN (";
      if (p.param_idxs.empty()) {
        throw std::runtime_error("orm compile: empty IN");
      }
      for (std::size_t i = 0; i < p.param_idxs.size(); ++i) {
        if (i) {
          out += ", ";
        }
        out += "%s";
        order.push_back(p.param_idxs[i]);
      }
      out += ')';
      break;
    }
    case CmpOp::IsNull:
      out += p.is_null_negated ? " IS NOT NULL" : " IS NULL";
      break;
  }
}

void emit_bool(std::string& out, std::vector<std::uint32_t>& order, DialectId d,
               const Query& q, const ModelSchema& m, const BoolExpr& e) {
  switch (e.kind) {
    case BoolExpr::Kind::Atom:
      emit_pred(out, order, d, q, m, e.atom);
      break;
    case BoolExpr::Kind::Not:
      out += "NOT (";
      if (e.children.size() != 1) {
        throw std::runtime_error("orm compile: Not arity");
      }
      emit_bool(out, order, d, q, m, e.children[0]);
      out += ')';
      break;
    case BoolExpr::Kind::And:
    case BoolExpr::Kind::Or: {
      if (e.children.empty()) {
        out += e.kind == BoolExpr::Kind::And ? "1=1" : "0=1";
        return;
      }
      const char* sep = e.kind == BoolExpr::Kind::And ? " AND " : " OR ";
      if (e.children.size() > 1) {
        out += '(';
      }
      for (std::size_t i = 0; i < e.children.size(); ++i) {
        if (i) {
          out += sep;
        }
        emit_bool(out, order, d, q, m, e.children[i]);
      }
      if (e.children.size() > 1) {
        out += ')';
      }
      break;
    }
    case BoolExpr::Kind::Xor: {
      // Portable rewrite (matches Django when backend lacks XOR):
      //   (c0 OR c1 OR ...) AND (1 = [MOD](sum(CASE WHEN ci THEN 1 ELSE 0), 2))
      if (e.children.empty()) {
        out += "0=1";
        return;
      }
      out += "((";
      for (std::size_t i = 0; i < e.children.size(); ++i) {
        if (i) {
          out += " OR ";
        }
        emit_bool(out, order, d, q, m, e.children[i]);
      }
      out += ") AND (1 = ";
      if (e.children.size() > 2) {
        out += "MOD(";
      }
      out += '(';
      for (std::size_t i = 0; i < e.children.size(); ++i) {
        if (i) {
          out += " + ";
        }
        out += "CASE WHEN ";
        emit_bool(out, order, d, q, m, e.children[i]);
        out += " THEN 1 ELSE 0 END";
      }
      out += ')';
      if (e.children.size() > 2) {
        out += ", 2)";
      }
      out += "))";
      break;
    }
  }
}

CompiledSql compile_select_inner(const Query& q, const ModelSchema& m) {
  CompiledSql result;
  const DialectId d = q.dialect;
  std::string& sql = result.sql;
  auto& order = result.param_order;

  sql = "SELECT ";
  if (q.distinct) {
    sql += "DISTINCT ";
  }

  std::vector<SelectItem> items = q.select;
  if (items.empty()) {
    for (const auto& f : m.fields) {
      if (f.is_relation() && f.column.empty()) {
        continue;
      }
      if (f.column.empty()) {
        continue;
      }
      SelectItem it;
      it.col.field = f.id;
      items.push_back(std::move(it));
    }
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) {
      sql += ", ";
    }
    emit_column(sql, d, q, m, items[i].col);
    if (!items[i].out_alias.empty()) {
      sql += " AS ";
      append_quoted(sql, d, items[i].out_alias);
    }
  }

  sql += " FROM ";
  append_quoted(sql, d, m.db_table);
  if (!q.base_alias.empty() && q.base_alias != m.db_table) {
    sql += ' ';
    append_quoted(sql, d, q.base_alias);
  }

  for (const auto& j : q.joins) {
    sql += j.type == JoinType::LeftOuter ? " LEFT OUTER JOIN " : " INNER JOIN ";
    append_quoted(sql, d, j.table);
    sql += ' ';
    append_quoted(sql, d, j.alias);
    sql += " ON ";
    append_quoted(sql, d, j.local_alias);
    sql += '.';
    append_quoted(sql, d, j.local_column);
    sql += " = ";
    append_quoted(sql, d, j.alias);
    sql += '.';
    append_quoted(sql, d, j.remote_column);
  }

  if (q.has_where) {
    sql += " WHERE ";
    emit_bool(sql, order, d, q, m, q.where);
  }

  if (!q.order_by.empty()) {
    sql += " ORDER BY ";
    for (std::size_t i = 0; i < q.order_by.size(); ++i) {
      if (i) {
        sql += ", ";
      }
      emit_column(sql, d, q, m, q.order_by[i].col);
      if (q.order_by[i].desc) {
        sql += " DESC";
      }
    }
  }

  if (q.limit.has_value()) {
    sql += " LIMIT ";
    sql += std::to_string(*q.limit);
  }
  if (q.offset > 0) {
    sql += " OFFSET ";
    sql += std::to_string(q.offset);
  }
  return result;
}

CompiledSql compile_update_inner(const Query& q, const ModelSchema& m) {
  CompiledSql result;
  if (q.assignments.empty()) {
    return result;
  }
  const DialectId d = q.dialect;
  std::string& sql = result.sql;
  auto& order = result.param_order;

  sql = "UPDATE ";
  append_quoted(sql, d, m.db_table);
  sql += " SET ";
  for (std::size_t i = 0; i < q.assignments.size(); ++i) {
    if (i) {
      sql += ", ";
    }
    auto* f = field_of(m, q.assignments[i].field);
    if (!f) {
      throw std::runtime_error("orm compile: bad update field");
    }
    append_quoted(sql, d, f->column);
    if (q.assignments[i].set_null) {
      sql += " = NULL";
    } else {
      sql += " = %s";
      order.push_back(q.assignments[i].param_idx);
    }
  }
  if (q.has_where) {
    sql += " WHERE ";
    emit_bool(sql, order, d, q, m, q.where);
  }
  return result;
}

CompiledSql compile_delete_inner(const Query& q, const ModelSchema& m) {
  CompiledSql result;
  const DialectId d = q.dialect;
  std::string& sql = result.sql;
  auto& order = result.param_order;

  sql = "DELETE FROM ";
  append_quoted(sql, d, m.db_table);
  if (q.has_where) {
    sql += " WHERE ";
    emit_bool(sql, order, d, q, m, q.where);
  }
  return result;
}

}  // namespace

std::string quote_ident(DialectId d, std::string_view name) {
  const char q = (d == DialectId::MySQL) ? '`' : '"';
  std::string out;
  out.reserve(name.size() + 2);
  out += q;
  for (char c : name) {
    if (c == q) {
      out += q;
      out += q;
    } else {
      out += c;
    }
  }
  out += q;
  return out;
}

DialectId dialect_from_vendor(std::string_view vendor) {
  if (vendor == "postgresql" || vendor == "postgres") {
    return DialectId::Postgres;
  }
  if (vendor == "mysql" || vendor == "mariadb") {
    return DialectId::MySQL;
  }
  return DialectId::SQLite;
}

CompiledSql compile_query(const Query& q, const SchemaRegistry& reg) {
  const ModelSchema* model = reg.get(q.model);
  if (!model) {
    return {};
  }
  try {
    switch (q.kind) {
      case StmtKind::Select:
        return compile_select_inner(q, *model);
      case StmtKind::Update:
        return compile_update_inner(q, *model);
      case StmtKind::Delete:
        return compile_delete_inner(q, *model);
    }
  } catch (...) {
    return {};
  }
  return {};
}

}  // namespace django::orm
