// Nanobind surface for the ORM data plane.
#include "orm_engine/compile.hpp"
#include "orm_engine/queryset.hpp"
#include "orm_engine/schema.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace {

django::orm::ParamValue param_from_python(nb::handle h) {
  if (h.is_none()) {
    return django::orm::ParamValue::null();
  }
  if (nb::isinstance<nb::bool_>(h)) {
    return django::orm::ParamValue::from_bool(nb::cast<bool>(h));
  }
  if (nb::isinstance<nb::int_>(h)) {
    return django::orm::ParamValue::from_int(nb::cast<std::int64_t>(h));
  }
  if (nb::isinstance<nb::float_>(h)) {
    return django::orm::ParamValue::from_float(nb::cast<double>(h));
  }
  if (nb::isinstance<nb::str>(h)) {
    return django::orm::ParamValue::from_string(nb::cast<std::string>(h));
  }
  if (nb::isinstance<nb::list>(h) || nb::isinstance<nb::tuple>(h)) {
    // For IN, caller expands; single value fallback
    return django::orm::ParamValue::from_string(nb::cast<std::string>(nb::str(h)));
  }
  return django::orm::ParamValue::from_string(nb::cast<std::string>(nb::str(h)));
}

nb::object param_to_python(const django::orm::ParamValue& p) {
  using K = django::orm::ParamValue::Kind;
  switch (p.kind) {
    case K::None:
      return nb::none();
    case K::Bool:
      return nb::bool_(p.b);
    case K::Int:
      return nb::int_(p.i);
    case K::Float:
      return nb::float_(p.f);
    case K::String:
      return nb::str(p.s.c_str(), p.s.size());
    case K::Bytes:
      return nb::bytes(reinterpret_cast<const char*>(p.bytes.data()),
                       p.bytes.size());
  }
  return nb::none();
}

nb::tuple compile_to_tuple(const django::orm::QuerySet& self) {
  auto compiled = self.compile();
  nb::list params;
  const auto& q = self.query();
  for (auto idx : compiled.param_order) {
    if (idx >= q.params.size()) {
      throw std::runtime_error("param index out of range");
    }
    params.append(param_to_python(q.params[idx]));
  }
  return nb::make_tuple(nb::str(compiled.sql.c_str(), compiled.sql.size()), params);
}

// Python tree dict:
//   {"kind": "and"|"or"|"not"|"xor"|"atom", "children": [...],
//    "key": str, "values": list,
//    "rhs_sql": str, "rhs_params": list, "rhs_op": int}  # subquery atom
django::orm::QNode q_node_from_python(nb::handle h) {
  django::orm::QNode node;
  nb::dict d = nb::cast<nb::dict>(h);
  std::string kind = nb::cast<std::string>(d["kind"]);
  if (kind == "and") {
    node.kind = 0;
  } else if (kind == "or") {
    node.kind = 1;
  } else if (kind == "not") {
    node.kind = 2;
  } else if (kind == "xor") {
    node.kind = 4;
  } else if (kind == "atom") {
    node.kind = 3;
    node.key = nb::cast<std::string>(d["key"]);
    if (d.contains("rhs_sql")) {
      node.has_rhs_sql = true;
      node.rhs_sql = nb::cast<std::string>(d["rhs_sql"]);
      node.rhs_op = d.contains("rhs_op") ? nb::cast<int>(d["rhs_op"]) : 0;
      if (d.contains("rhs_params")) {
        for (nb::handle x : nb::borrow(d["rhs_params"])) {
          node.rhs_params.push_back(param_from_python(x));
        }
      }
    }
    if (d.contains("values")) {
      nb::object vals = d["values"];
      if (nb::isinstance<nb::list>(vals) || nb::isinstance<nb::tuple>(vals)) {
        for (nb::handle x : nb::borrow(vals)) {
          node.values.push_back(param_from_python(x));
        }
      } else {
        node.values.push_back(param_from_python(vals));
      }
    }
    return node;
  } else {
    throw std::runtime_error("unknown Q node kind: " + kind);
  }
  if (d.contains("children")) {
    for (nb::handle c : nb::borrow(d["children"])) {
      node.children.push_back(q_node_from_python(c));
    }
  }
  return node;
}

}  // namespace

void register_orm_engine(nb::module_& parent) {
  nb::module_ m = parent.def_submodule("orm", "Native ORM data plane");

  m.def("clear_schema",
        []() { django::orm::SchemaRegistry::instance().clear(); });

  m.def(
      "register_model",
      [](const std::string& label, const std::string& db_table, nb::list fields) {
        // fields tuple (min 6, full 14):
        // 0 name, 1 attname, 2 column, 3 class_name, 4 pk, 5 null,
        // 6 remote_table, 7 remote_pk, 8 remote_label,
        // 9 rel_kind ("fk"|"rev_fk"|"m2m"|"rev_m2m"|""),
        // 10 m2m_table, 11 m2m_column, 12 m2m_reverse_column, 13 remote_fk_column
        django::orm::ModelSchema schema;
        schema.label = label;
        schema.db_table = db_table;
        for (nb::handle item : fields) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          django::orm::FieldSchema f;
          f.name = nb::cast<std::string>(t[0]);
          f.attname = nb::cast<std::string>(t[1]);
          f.column = nb::cast<std::string>(t[2]);
          f.type = django::orm::field_type_from_class_name(
              nb::cast<std::string>(t[3]));
          f.primary_key = nb::cast<bool>(t[4]);
          f.nullable = nb::cast<bool>(t[5]);
          const Py_ssize_t n = nb::len(t);
          if (n >= 9) {
            f.remote_table = nb::cast<std::string>(t[6]);
            f.remote_pk_column = nb::cast<std::string>(t[7]);
            f.remote_model_label = nb::cast<std::string>(t[8]);
          }
          if (n >= 10) {
            f.rel = django::orm::rel_kind_from_string(nb::cast<std::string>(t[9]));
          } else if (!f.remote_table.empty()) {
            f.rel = django::orm::RelKind::ForwardFK;
          }
          if (n >= 13) {
            f.m2m_table = nb::cast<std::string>(t[10]);
            f.m2m_column = nb::cast<std::string>(t[11]);
            f.m2m_reverse_column = nb::cast<std::string>(t[12]);
          }
          if (n >= 14) {
            f.remote_fk_column = nb::cast<std::string>(t[13]);
          }
          if (f.rel != django::orm::RelKind::None) {
            f.type = django::orm::FieldType::ForeignKey;
          }
          schema.fields.push_back(std::move(f));
        }
        return static_cast<std::uint32_t>(
            django::orm::SchemaRegistry::instance().register_model(
                std::move(schema)));
      },
      nb::arg("label"), nb::arg("db_table"), nb::arg("fields"));

  m.def(
      "model_id",
      [](const std::string& label) -> nb::object {
        auto id = django::orm::SchemaRegistry::instance().find_id(label);
        if (!id) {
          return nb::none();
        }
        return nb::int_(static_cast<std::uint32_t>(*id));
      },
      nb::arg("label"));

  m.def(
      "dialect_from_vendor",
      [](const std::string& vendor) {
        return static_cast<int>(django::orm::dialect_from_vendor(vendor));
      },
      nb::arg("vendor"));

  m.attr("DIALECT_POSTGRES") = 0;
  m.attr("DIALECT_MYSQL") = 1;
  m.attr("DIALECT_SQLITE") = 2;

  nb::class_<django::orm::QuerySet>(m, "QuerySet")
      .def(nb::init<>())
      .def_static(
          "create",
          [](std::uint32_t model_id, int dialect) {
            return django::orm::QuerySet(
                static_cast<django::orm::ModelId>(model_id),
                static_cast<django::orm::DialectId>(dialect));
          },
          nb::arg("model_id"), nb::arg("dialect") = 0)
      .def("clone", &django::orm::QuerySet::clone)
      .def(
          "filter_eq",
          [](django::orm::QuerySet& self, const std::string& field,
             nb::handle value) {
            return self.filter_eq(field, param_from_python(value));
          },
          nb::arg("field"), nb::arg("value"))
      .def(
          "filter_cmp",
          [](django::orm::QuerySet& self, const std::string& field, int op,
             nb::handle value) {
            return self.filter_cmp(field, static_cast<django::orm::CmpOp>(op),
                                   param_from_python(value));
          },
          nb::arg("field"), nb::arg("op"), nb::arg("value"))
      .def(
          "filter_isnull",
          [](django::orm::QuerySet& self, const std::string& field, bool is_null) {
            return self.filter_isnull(field, is_null);
          },
          nb::arg("field"), nb::arg("is_null"))
      .def(
          "filter_in",
          [](django::orm::QuerySet& self, const std::string& field,
             nb::sequence values) {
            std::vector<django::orm::ParamValue> ps;
            for (nb::handle v : values) {
              ps.push_back(param_from_python(v));
            }
            return self.filter_in(field, std::move(ps));
          },
          nb::arg("field"), nb::arg("values"))
      .def(
          "filter_kwargs",
          [](django::orm::QuerySet& self, nb::dict kwargs, bool disjunctive) {
            std::vector<std::pair<std::string, std::vector<django::orm::ParamValue>>>
                items;
            for (auto kv : kwargs) {
              std::string key = nb::cast<std::string>(nb::str(kv.first));
              nb::handle val = kv.second;
              std::vector<django::orm::ParamValue> ps;
              if (nb::isinstance<nb::list>(val) || nb::isinstance<nb::tuple>(val)) {
                nb::object seq = nb::borrow(val);
                for (nb::handle x : seq) {
                  ps.push_back(param_from_python(x));
                }
              } else {
                ps.push_back(param_from_python(val));
              }
              items.emplace_back(std::move(key), std::move(ps));
            }
            return self.filter_kwargs(items, disjunctive);
          },
          nb::arg("kwargs"), nb::arg("disjunctive") = false)
      .def(
          "apply_q",
          [](django::orm::QuerySet& self, nb::dict tree) {
            return self.apply_q(q_node_from_python(tree));
          },
          nb::arg("tree"),
          "Apply a full Q-tree dict (and/or/not/xor/atom) in C++.")
      .def(
          "filter_subquery",
          [](django::orm::QuerySet& self, const std::string& field, int op,
             const std::string& sql, nb::sequence params) {
            std::vector<django::orm::ParamValue> ps;
            for (nb::handle v : params) {
              ps.push_back(param_from_python(v));
            }
            return self.filter_subquery(field, static_cast<django::orm::CmpOp>(op),
                                        sql, std::move(ps));
          },
          nb::arg("field"), nb::arg("op"), nb::arg("sql"), nb::arg("params"))
      .def(
          "values_list",
          [](django::orm::QuerySet& self, nb::sequence names, bool flat) {
            std::vector<std::string> ns;
            for (nb::handle n : names) {
              ns.push_back(nb::cast<std::string>(n));
            }
            auto mode = flat ? django::orm::ResultMode::ValuesListFlat
                             : django::orm::ResultMode::ValuesList;
            return self.values(ns, true, mode);
          },
          nb::arg("names"), nb::arg("flat") = false)
      .def(
          "values",
          [](django::orm::QuerySet& self, nb::sequence names) {
            std::vector<std::string> ns;
            for (nb::handle n : names) {
              ns.push_back(nb::cast<std::string>(n));
            }
            return self.values(ns, true, django::orm::ResultMode::ValuesDict);
          },
          nb::arg("names"))
      .def("clear_select", &django::orm::QuerySet::clear_select)
      .def("select_model_columns", &django::orm::QuerySet::select_model_columns)
      .def(
          "annotate_aggregate",
          [](django::orm::QuerySet& self, const std::string& alias,
             const std::string& func, const std::string& field, bool distinct,
             bool star) {
            return self.annotate_aggregate(alias, func, field, distinct, star);
          },
          nb::arg("alias"), nb::arg("func"), nb::arg("field") = "",
          nb::arg("distinct") = false, nb::arg("star") = false)
      .def(
          "annotate_sql",
          [](django::orm::QuerySet& self, const std::string& alias,
             const std::string& sql, nb::sequence params) {
            std::vector<django::orm::ParamValue> ps;
            for (nb::handle v : params) {
              ps.push_back(param_from_python(v));
            }
            return self.annotate_sql(alias, sql, std::move(ps));
          },
          nb::arg("alias"), nb::arg("sql"), nb::arg("params"))
      .def(
          "group_by_fields",
          [](django::orm::QuerySet& self, nb::sequence names) {
            std::vector<std::string> ns;
            for (nb::handle n : names) {
              ns.push_back(nb::cast<std::string>(n));
            }
            return self.group_by_fields(ns);
          },
          nb::arg("names"))
      .def("group_by_selected_columns",
           &django::orm::QuerySet::group_by_selected_columns)
      .def(
          "add_select_related",
          [](django::orm::QuerySet& self, const std::string& path) {
            return self.add_select_related(path);
          },
          nb::arg("path"))
      .def(
          "add_select_related_all",
          [](django::orm::QuerySet& self, int max_depth) {
            return self.add_select_related_all(max_depth);
          },
          nb::arg("max_depth") = 5)
      .def(
          "add_prefetch",
          [](django::orm::QuerySet& self, const std::string& lookup) {
            return self.add_prefetch(lookup);
          },
          nb::arg("lookup"))
      .def(
          "filter_subquery_qs",
          [](django::orm::QuerySet& self, const std::string& field, int op,
             const django::orm::QuerySet& sub) {
            return self.filter_subquery_qs(
                field, static_cast<django::orm::CmpOp>(op), sub);
          },
          nb::arg("field"), nb::arg("op"), nb::arg("sub"))
      .def(
          "annotate_subquery_qs",
          [](django::orm::QuerySet& self, const std::string& alias,
             const django::orm::QuerySet& sub) {
            return self.annotate_subquery_qs(alias, sub);
          },
          nb::arg("alias"), nb::arg("sub"))
      .def(
          "annotate_case",
          [](django::orm::QuerySet& self, const std::string& alias,
             nb::list cases, nb::object else_val) {
            // cases: list of (when_dict, then_value)
            std::vector<std::pair<django::orm::QNode, django::orm::ParamValue>>
                parsed;
            for (nb::handle item : cases) {
              nb::tuple t = nb::cast<nb::tuple>(item);
              django::orm::QNode when = q_node_from_python(t[0]);
              parsed.emplace_back(std::move(when), param_from_python(t[1]));
            }
            bool has_else = !else_val.is_none();
            django::orm::ParamValue ev =
                has_else ? param_from_python(else_val)
                         : django::orm::ParamValue::null();
            return self.annotate_case(alias, parsed, has_else, std::move(ev));
          },
          nb::arg("alias"), nb::arg("cases"), nb::arg("else_val") = nb::none())
      .def(
          "annotate_binop",
          [](django::orm::QuerySet& self, const std::string& alias,
             const std::string& lhs, const std::string& op, nb::handle rhs) {
            return self.annotate_binop(alias, lhs, op, param_from_python(rhs));
          },
          nb::arg("alias"), nb::arg("lhs"), nb::arg("op"), nb::arg("rhs"))
      .def(
          "annotate_binop_fields",
          [](django::orm::QuerySet& self, const std::string& alias,
             const std::string& lhs, const std::string& op,
             const std::string& rhs) {
            return self.annotate_binop_fields(alias, lhs, op, rhs);
          },
          nb::arg("alias"), nb::arg("lhs"), nb::arg("op"), nb::arg("rhs"))
      .def(
          "annotate_value",
          [](django::orm::QuerySet& self, const std::string& alias,
             nb::handle value) {
            return self.annotate_value(alias, param_from_python(value));
          },
          nb::arg("alias"), nb::arg("value"))
      .def(
          "annotate_f",
          [](django::orm::QuerySet& self, const std::string& alias,
             const std::string& field) {
            return self.annotate_f(alias, field);
          },
          nb::arg("alias"), nb::arg("field"))
      .def(
          "compile_prefetch_secondary",
          [](const django::orm::QuerySet& self, nb::dict spec,
             nb::sequence parent_pks) {
            django::orm::PrefetchSpec p;
            p.lookup = nb::cast<std::string>(spec["lookup"]);
            if (spec.contains("parent_path")) {
              p.parent_path = nb::cast<std::string>(spec["parent_path"]);
            }
            if (spec.contains("hop")) {
              p.hop = nb::cast<std::string>(spec["hop"]);
            }
            p.rel = django::orm::rel_kind_from_string(
                nb::cast<std::string>(spec["rel"]));
            p.remote_table = nb::cast<std::string>(spec["remote_table"]);
            p.remote_model_label =
                nb::cast<std::string>(spec["remote_model_label"]);
            p.remote_pk_column =
                nb::cast<std::string>(spec["remote_pk_column"]);
            p.remote_fk_column =
                nb::cast<std::string>(spec["remote_fk_column"]);
            p.m2m_table = nb::cast<std::string>(spec["m2m_table"]);
            p.m2m_column = nb::cast<std::string>(spec["m2m_column"]);
            p.m2m_reverse_column =
                nb::cast<std::string>(spec["m2m_reverse_column"]);
            p.parent_pk_column =
                nb::cast<std::string>(spec["parent_pk_column"]);
            p.cache_name = nb::cast<std::string>(spec["cache_name"]);
            std::vector<django::orm::ParamValue> pks;
            for (nb::handle v : parent_pks) {
              pks.push_back(param_from_python(v));
            }
            auto compiled = self.compile_prefetch_secondary(p, pks);
            nb::list params;
            for (const auto& pv : compiled.params) {
              params.append(param_to_python(pv));
            }
            // (sql, params, parent_link_offset)
            return nb::make_tuple(
                nb::str(compiled.sql.c_str(), compiled.sql.size()), params,
                compiled.parent_link_offset);
          },
          nb::arg("spec"), nb::arg("parent_pks"))
      .def(
          "prefetch_specs",
          [](const django::orm::QuerySet& self) {
            nb::list out;
            for (const auto& p : self.prefetches()) {
              nb::dict d;
              d["lookup"] = p.lookup;
              d["parent_path"] = p.parent_path;
              d["hop"] = p.hop;
              // string form for rel_kind_from_string
              const char* rels[] = {"", "fk", "rev_fk", "m2m", "rev_m2m"};
              int ri = static_cast<int>(p.rel);
              d["rel"] = (ri >= 0 && ri <= 4) ? rels[ri] : "";
              d["remote_table"] = p.remote_table;
              d["remote_model_label"] = p.remote_model_label;
              d["remote_pk_column"] = p.remote_pk_column;
              d["remote_fk_column"] = p.remote_fk_column;
              d["m2m_table"] = p.m2m_table;
              d["m2m_column"] = p.m2m_column;
              d["m2m_reverse_column"] = p.m2m_reverse_column;
              d["parent_pk_column"] = p.parent_pk_column;
              d["cache_name"] = p.cache_name;
              out.append(d);
            }
            return out;
          })
      .def(
          "annotation_selects",
          [](const django::orm::QuerySet& self) {
            nb::list out;
            for (const auto& a : self.annotation_selects()) {
              nb::dict d;
              d["alias"] = a.alias;
              d["offset"] = a.offset;
              out.append(d);
            }
            return out;
          })
      .def(
          "add_update",
          [](django::orm::QuerySet& self, const std::string& field,
             nb::handle value) {
            return self.add_update(field, param_from_python(value));
          },
          nb::arg("field"), nb::arg("value"))
      .def("set_delete", &django::orm::QuerySet::set_delete)
      .def(
          "order_by",
          [](django::orm::QuerySet& self, const std::string& field, bool desc) {
            return self.order_by(field, desc);
          },
          nb::arg("field"), nb::arg("desc") = false)
      .def(
          "order_by_alias",
          [](django::orm::QuerySet& self, const std::string& alias, bool desc) {
            return self.order_by_alias(alias, desc);
          },
          nb::arg("alias"), nb::arg("desc") = false)
      .def("set_limit", &django::orm::QuerySet::set_limit, nb::arg("n"))
      .def("set_offset", &django::orm::QuerySet::set_offset, nb::arg("n"))
      .def("set_distinct", &django::orm::QuerySet::set_distinct, nb::arg("v"))
      .def("compile_sql", &compile_to_tuple)
      .def("base_attnames",
           [](const django::orm::QuerySet& self) {
             return self.base_attnames();
           })
      .def("related_selects_info",
           [](const django::orm::QuerySet& self) {
             nb::list out;
             for (const auto& rs : self.related_selects()) {
               nb::dict d;
               d["path"] = rs.path;
               d["model_id"] = rs.model_id;
               d["offset"] = rs.select_offset;
               d["count"] = rs.select_count;
               nb::list atts;
               for (const auto& a : rs.field_attnames) {
                 atts.append(a);
               }
               d["attnames"] = atts;
               out.append(d);
             }
             return out;
           })
      .def("prefetch_lookups", [](const django::orm::QuerySet& self) {
        // Unique full lookup paths (multi-hop expands to multiple specs).
        nb::list out;
        std::vector<std::string> seen;
        for (const auto& p : self.prefetches()) {
          if (std::find(seen.begin(), seen.end(), p.lookup) != seen.end()) {
            continue;
          }
          seen.push_back(p.lookup);
          out.append(p.lookup);
        }
        return out;
      });

  // CmpOp constants
  m.attr("OP_EQ") = 0;
  m.attr("OP_NE") = 1;
  m.attr("OP_LT") = 2;
  m.attr("OP_LTE") = 3;
  m.attr("OP_GT") = 4;
  m.attr("OP_GTE") = 5;
  m.attr("OP_IN") = 6;
  m.attr("OP_ISNULL") = 7;
}
