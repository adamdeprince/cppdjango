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

std::string table_alias(const ModelSchema& m, const ColumnRef& c) {
  if (!c.table_alias.empty()) {
    return c.table_alias;
  }
  return m.db_table;
}

void emit_column(std::string& out, DialectId d, const ModelSchema& m,
                 const ColumnRef& c) {
  auto* f = field_of(m, c.field);
  if (!f) {
    throw std::runtime_error("orm compile: bad field id");
  }
  append_quoted(out, d, table_alias(m, c));
  out += '.';
  append_quoted(out, d, f->column);
}

void emit_pred(std::string& out, std::vector<std::uint32_t>& order, DialectId d,
               const ModelSchema& m, const Pred& p) {
  emit_column(out, d, m, p.lhs);
  switch (p.op) {
    case CmpOp::Eq:
      out += " = %s";
      if (p.param_idxs.size() != 1) {
        throw std::runtime_error("orm compile: Eq needs 1 param");
      }
      order.push_back(p.param_idxs[0]);
      break;
    case CmpOp::Ne:
      out += " <> %s";
      if (p.param_idxs.size() != 1) {
        throw std::runtime_error("orm compile: Ne needs 1 param");
      }
      order.push_back(p.param_idxs[0]);
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
               const ModelSchema& m, const BoolExpr& e) {
  switch (e.kind) {
    case BoolExpr::Kind::Atom:
      emit_pred(out, order, d, m, e.atom);
      break;
    case BoolExpr::Kind::Not:
      out += "NOT (";
      if (e.children.size() != 1) {
        throw std::runtime_error("orm compile: Not arity");
      }
      emit_bool(out, order, d, m, e.children[0]);
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
        emit_bool(out, order, d, m, e.children[i]);
      }
      if (e.children.size() > 1) {
        out += ')';
      }
      break;
    }
  }
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

CompiledSql compile_select(const Query& q, const SchemaRegistry& reg) {
  CompiledSql result;
  const ModelSchema* model = reg.get(q.model);
  if (!model) {
    return result;
  }
  const DialectId d = q.dialect;
  std::string& sql = result.sql;
  auto& order = result.param_order;

  sql = "SELECT ";
  if (q.distinct) {
    sql += "DISTINCT ";
  }

  std::vector<SelectItem> items = q.select;
  if (items.empty()) {
    for (const auto& f : model->fields) {
      SelectItem it;
      it.col.field = f.id;
      items.push_back(std::move(it));
    }
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) {
      sql += ", ";
    }
    emit_column(sql, d, *model, items[i].col);
    if (!items[i].out_alias.empty()) {
      sql += " AS ";
      append_quoted(sql, d, items[i].out_alias);
    }
  }

  sql += " FROM ";
  append_quoted(sql, d, model->db_table);

  if (q.has_where) {
    sql += " WHERE ";
    emit_bool(sql, order, d, *model, q.where);
  }

  if (!q.order_by.empty()) {
    sql += " ORDER BY ";
    for (std::size_t i = 0; i < q.order_by.size(); ++i) {
      if (i) {
        sql += ", ";
      }
      emit_column(sql, d, *model, q.order_by[i].col);
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
    // SQLite/Postgres/MySQL
    if (!q.limit.has_value() && d == DialectId::MySQL) {
      // MySQL offset-only needs huge limit — use dialect convention later.
    }
    sql += " OFFSET ";
    sql += std::to_string(q.offset);
  }

  return result;
}

}  // namespace django::orm
