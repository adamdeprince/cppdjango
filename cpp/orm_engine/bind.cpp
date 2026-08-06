// Nanobind surface for the ORM data plane.
#include "orm_engine/compile.hpp"
#include "orm_engine/queryset.hpp"
#include "orm_engine/schema.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

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
//   {"kind": "and"|"or"|"not"|"atom", "children": [...], "key": str, "values": list}
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
  } else if (kind == "atom") {
    node.kind = 3;
    node.key = nb::cast<std::string>(d["key"]);
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
        // fields: (name, attname, column, class_name, pk, null,
        //          remote_table, remote_pk_col, remote_label)  — last 3 optional
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
          if (nb::len(t) >= 9) {
            f.remote_table = nb::cast<std::string>(t[6]);
            f.remote_pk_column = nb::cast<std::string>(t[7]);
            f.remote_model_label = nb::cast<std::string>(t[8]);
            f.is_relation = !f.remote_table.empty();
            if (f.is_relation) {
              f.type = django::orm::FieldType::ForeignKey;
            }
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
          "Apply a full Q-tree dict (and/or/not/atom) in C++.")
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
      .def("set_limit", &django::orm::QuerySet::set_limit, nb::arg("n"))
      .def("set_offset", &django::orm::QuerySet::set_offset, nb::arg("n"))
      .def("compile_sql", &compile_to_tuple);

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
