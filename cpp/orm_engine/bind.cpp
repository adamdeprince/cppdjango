// Nanobind surface for the ORM data plane.
#include "orm_engine/compile.hpp"
#include "orm_engine/queryset.hpp"
#include "orm_engine/schema.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
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

django::orm::QuerySet make_queryset(std::uint32_t model_id, int dialect) {
  auto d = static_cast<django::orm::DialectId>(dialect);
  return django::orm::QuerySet(static_cast<django::orm::ModelId>(model_id), d);
}

}  // namespace

void register_orm_engine(nb::module_& parent) {
  nb::module_ m = parent.def_submodule("orm", "Native ORM data plane");

  m.def("clear_schema",
        []() { django::orm::SchemaRegistry::instance().clear(); });

  m.def(
      "register_model",
      [](const std::string& label, const std::string& db_table, nb::list fields) {
        // fields: sequence of (name, attname, column, class_name, pk, nullable)
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

  // Dialect constants (ints) — avoid nanobind enum issues at import time.
  m.attr("DIALECT_POSTGRES") = 0;
  m.attr("DIALECT_MYSQL") = 1;
  m.attr("DIALECT_SQLITE") = 2;

  nb::class_<django::orm::QuerySet>(m, "QuerySet")
      .def(nb::init<>())
      .def_static(
          "create",
          [](std::uint32_t model_id, int dialect) {
            return make_queryset(model_id, dialect);
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
          "values_list",
          [](django::orm::QuerySet& self, nb::sequence names, bool flat) {
            std::vector<std::string> ns;
            for (nb::handle n : names) {
              ns.push_back(nb::cast<std::string>(n));
            }
            auto mode = flat ? django::orm::ResultMode::ValuesListFlat
                             : django::orm::ResultMode::ValuesList;
            return self.values(ns, /*out_aliases=*/true, mode);
          },
          nb::arg("names"), nb::arg("flat") = false)
      .def(
          "values",
          [](django::orm::QuerySet& self, nb::sequence names) {
            std::vector<std::string> ns;
            for (nb::handle n : names) {
              ns.push_back(nb::cast<std::string>(n));
            }
            return self.values(ns, /*out_aliases=*/true,
                               django::orm::ResultMode::ValuesDict);
          },
          nb::arg("names"))
      .def(
          "set_limit",
          [](django::orm::QuerySet& self, std::uint64_t n) { self.set_limit(n); },
          nb::arg("n"))
      .def(
          "set_offset",
          [](django::orm::QuerySet& self, std::uint64_t n) {
            self.set_offset(n);
          },
          nb::arg("n"))
      .def("compile_sql", [](const django::orm::QuerySet& self) {
        auto compiled = self.compile();
        nb::list params;
        const auto& q = self.query();
        for (auto idx : compiled.param_order) {
          if (idx >= q.params.size()) {
            throw std::runtime_error("param index out of range");
          }
          params.append(param_to_python(q.params[idx]));
        }
        return nb::make_tuple(nb::str(compiled.sql.c_str(), compiled.sql.size()),
                              params);
      });
}
