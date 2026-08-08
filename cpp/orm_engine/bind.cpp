// Nanobind surface for the ORM data plane.
#include "orm_engine/compile.hpp"
#include "orm_engine/queryset.hpp"
#include "orm_engine/schema.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "nb_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace {

std::mutex g_simple_compile_cache_mutex;
std::unordered_map<std::string, std::string> g_simple_compile_cache;
std::atomic<std::uint64_t> g_simple_compile_cache_hits{0};
std::atomic<std::uint64_t> g_simple_compile_cache_misses{0};

void append_key_part(std::string& key, std::string_view part) {
  key += std::to_string(part.size());
  key += ':';
  key.append(part);
  key += '|';
}

std::string simple_compile_key(
    char kind, std::uint32_t model_id, int dialect,
    const std::vector<std::string>& fields, std::string_view lookup,
    const std::vector<std::string>& ordering, std::uint64_t limit,
    std::uint64_t offset, bool null_lookup = false) {
  std::string key;
  key.reserve(64 + fields.size() * 16 + ordering.size() * 16);
  key += kind;
  key += '|';
  key += std::to_string(
      django::orm::SchemaRegistry::instance().generation());
  key += '|';
  key += std::to_string(model_id);
  key += '|';
  key += std::to_string(dialect);
  key += '|';
  key += std::to_string(limit);
  key += '|';
  key += std::to_string(offset);
  key += '|';
  key += null_lookup ? "null|" : "value|";
  append_key_part(key, lookup);
  for (const auto& field : fields) {
    append_key_part(key, field);
  }
  key += "#|";
  for (const auto& item : ordering) {
    append_key_part(key, item);
  }
  return key;
}

template <typename Builder>
std::string cached_simple_sql(const std::string& key, Builder&& builder) {
  {
    std::lock_guard lock(g_simple_compile_cache_mutex);
    if (auto it = g_simple_compile_cache.find(key);
        it != g_simple_compile_cache.end()) {
      ++g_simple_compile_cache_hits;
      return it->second;
    }
  }
  std::string sql = builder();
  if (sql.empty()) {
    return {};
  }
  std::lock_guard lock(g_simple_compile_cache_mutex);
  auto [it, inserted] = g_simple_compile_cache.emplace(key, std::move(sql));
  if (inserted) {
    ++g_simple_compile_cache_misses;
  } else {
    ++g_simple_compile_cache_hits;
  }
  return it->second;
}

void clear_simple_compile_cache() {
  std::lock_guard lock(g_simple_compile_cache_mutex);
  g_simple_compile_cache.clear();
  g_simple_compile_cache_hits = 0;
  g_simple_compile_cache_misses = 0;
}

std::vector<std::string> strings_from_sequence(nb::sequence values) {
  std::vector<std::string> out;
  out.reserve(nb::len(values));
  for (nb::handle value : values) {
    out.push_back(nb::cast<std::string>(value));
  }
  return out;
}

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

nb::object postgres_integer_to_python(
    const django::orm::ParamValue& p, nb::handle integer_types) {
  using H = django::orm::ParamValue::TypeHint;
  std::size_t index = 0;
  switch (p.type_hint) {
    case H::PostgresInt2:
      index = 0;
      break;
    case H::PostgresInt4:
      index = 1;
      break;
    case H::PostgresInt8:
      index = 2;
      break;
    case H::Default:
      return nb::int_(p.i);
  }
  if (integer_types.is_none()) {
    return nb::int_(p.i);
  }
  if (!nb::isinstance<nb::tuple>(integer_types) || nb::len(integer_types) != 3) {
    throw nb::type_error("PostgreSQL integer types must be a 3-tuple");
  }
  PyObject* type_object = PyTuple_GET_ITEM(integer_types.ptr(), index);
  if (!PyType_Check(type_object) ||
      !PyType_IsSubtype(reinterpret_cast<PyTypeObject*>(type_object),
                        &PyLong_Type)) {
    throw nb::type_error("PostgreSQL integer adapter must subclass int");
  }

  // psycopg's Int2/Int4/Int8 are slotless int subclasses whose Python
  // __new__ methods delegate directly to int.__new__. Call that C slot
  // ourselves so native preparation doesn't bounce through Python bytecode.
  PyObject* raw_value = PyLong_FromLongLong(p.i);
  if (!raw_value) {
    throw nb::python_error();
  }
  PyObject* args = PyTuple_New(1);
  if (!args) {
    Py_DECREF(raw_value);
    throw nb::python_error();
  }
  PyTuple_SET_ITEM(args, 0, raw_value);  // Steals raw_value.
  PyObject* wrapped = PyLong_Type.tp_new(
      reinterpret_cast<PyTypeObject*>(type_object), args, nullptr);
  Py_DECREF(args);
  if (!wrapped) {
    throw nb::python_error();
  }
  return nb::steal<nb::object>(wrapped);
}

nb::object param_to_python(
    const django::orm::ParamValue& p,
    nb::handle postgres_integer_types = nb::none()) {
  using K = django::orm::ParamValue::Kind;
  switch (p.kind) {
    case K::None:
      return nb::none();
    case K::Bool:
      return nb::bool_(p.b);
    case K::Int:
      return postgres_integer_to_python(p, postgres_integer_types);
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

const django::orm::ModelSchema* registered_model(std::uint32_t model_id) {
  return django::orm::SchemaRegistry::instance().get(
      static_cast<django::orm::ModelId>(model_id));
}

const django::orm::FieldSchema* registered_native_field(
    const django::orm::ModelSchema& model, std::string_view name) {
  auto it = model.field_by_name.find(std::string(name));
  if (it == model.field_by_name.end() || it->second >= model.fields.size()) {
    return nullptr;
  }
  const auto& field = model.fields[it->second];
  return field.is_native_scalar() ? &field : nullptr;
}

std::optional<django::orm::ParamValue> direct_param_from_python(
    nb::handle value, const django::orm::FieldSchema& field,
    bool allow_null = false) {
  using django::orm::FieldType;
  using django::orm::ParamValue;

  if (value.is_none()) {
    if (allow_null) {
      return ParamValue::null();
    }
    return std::nullopt;
  }

  const auto integer_value = [&]() -> std::optional<ParamValue> {
    if (!PyLong_CheckExact(value.ptr())) {
      return std::nullopt;
    }
    int overflow = 0;
    const long long converted = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (overflow != 0 || PyErr_Occurred()) {
      PyErr_Clear();
      return std::nullopt;
    }
    ParamValue::TypeHint type_hint = ParamValue::TypeHint::Default;
    switch (field.type) {
      case FieldType::SmallInteger:
        type_hint = ParamValue::TypeHint::PostgresInt2;
        break;
      case FieldType::Integer:
        type_hint = ParamValue::TypeHint::PostgresInt4;
        break;
      case FieldType::BigInteger:
        type_hint = ParamValue::TypeHint::PostgresInt8;
        break;
      default:
        break;
    }
    return ParamValue::from_int(
        static_cast<std::int64_t>(converted), type_hint);
  };

  switch (field.type) {
    case FieldType::Integer:
    case FieldType::BigInteger:
    case FieldType::SmallInteger:
    case FieldType::Auto:
    case FieldType::BigAuto:
      return integer_value();
    case FieldType::Float: {
      if (PyFloat_CheckExact(value.ptr())) {
        return ParamValue::from_float(PyFloat_AS_DOUBLE(value.ptr()));
      }
      auto integer = integer_value();
      if (!integer) {
        return std::nullopt;
      }
      return ParamValue::from_float(static_cast<double>(integer->i));
    }
    case FieldType::Boolean:
      if (!PyBool_Check(value.ptr())) {
        return std::nullopt;
      }
      return ParamValue::from_bool(value.ptr() == Py_True);
    case FieldType::Text:
    case FieldType::Char:
      if (!PyUnicode_CheckExact(value.ptr())) {
        return std::nullopt;
      }
      return ParamValue::from_string(nb::cast<std::string>(value));
    default:
      return std::nullopt;
  }
}

struct NativeLookup {
  const django::orm::FieldSchema* field = nullptr;
  std::string field_name;
  std::string lookup = "exact";
};

std::optional<NativeLookup> registered_native_lookup(
    const django::orm::ModelSchema& model, std::string_view key) {
  if (key.empty()) {
    return std::nullopt;
  }
  NativeLookup out;
  const auto separator = key.find("__");
  if (separator == std::string_view::npos) {
    out.field_name = std::string(key);
  } else {
    if (key.find("__", separator + 2) != std::string_view::npos) {
      return std::nullopt;
    }
    out.field_name = std::string(key.substr(0, separator));
    out.lookup = std::string(key.substr(separator + 2));
    if (out.lookup != "exact" && out.lookup != "gt" &&
        out.lookup != "gte" && out.lookup != "lt" &&
        out.lookup != "lte" && out.lookup != "in" &&
        out.lookup != "isnull") {
      return std::nullopt;
    }
  }
  if (out.field_name.empty()) {
    return std::nullopt;
  }
  out.field = registered_native_field(model, out.field_name);
  if (!out.field) {
    return std::nullopt;
  }
  return out;
}

bool validate_native_projection(
    const django::orm::ModelSchema& model,
    const std::vector<std::string>& fields) {
  if (fields.empty()) {
    return false;
  }
  return std::all_of(fields.begin(), fields.end(), [&](const auto& name) {
    return name.find("__") == std::string::npos &&
           registered_native_field(model, name) != nullptr;
  });
}

bool validate_native_ordering(
    const django::orm::ModelSchema& model,
    const std::vector<std::string>& ordering) {
  return std::all_of(ordering.begin(), ordering.end(), [&](const auto& item) {
    std::string_view name(item);
    if (!name.empty() && name.front() == '-') {
      name.remove_prefix(1);
    }
    return !name.empty() && name != "?" &&
           name.find("__") == std::string_view::npos &&
           registered_native_field(model, name) != nullptr;
  });
}

std::optional<django::orm::QNode> native_q_node_from_python(
    const django::orm::ModelSchema& model, nb::handle value,
    bool negated = false) {
  if (!nb::isinstance<nb::dict>(value)) {
    return std::nullopt;
  }
  nb::dict tree = nb::cast<nb::dict>(value);
  if (!tree.contains("kind") || !PyUnicode_CheckExact(tree["kind"].ptr())) {
    return std::nullopt;
  }
  const std::string kind = nb::cast<std::string>(tree["kind"]);
  django::orm::QNode node;
  if (kind == "atom") {
    node.kind = 3;
    if (!tree.contains("key") || !PyUnicode_CheckExact(tree["key"].ptr()) ||
        tree.contains("rhs_sql")) {
      return std::nullopt;
    }
    node.key = nb::cast<std::string>(tree["key"]);
    auto lookup = registered_native_lookup(model, node.key);
    if (!lookup || (negated && lookup->field->nullable) ||
        !tree.contains("values")) {
      return std::nullopt;
    }

    nb::handle raw_values = tree["values"];
    const bool sequence = nb::isinstance<nb::list>(raw_values) ||
                          nb::isinstance<nb::tuple>(raw_values);
    if (lookup->lookup == "isnull") {
      if (!sequence || nb::len(raw_values) != 1) {
        return std::nullopt;
      }
      nb::sequence raw_sequence = nb::cast<nb::sequence>(raw_values);
      nb::handle raw = raw_sequence[0];
      if (!PyBool_Check(raw.ptr())) {
        return std::nullopt;
      }
      node.values.push_back(
          django::orm::ParamValue::from_bool(raw.ptr() == Py_True));
      return node;
    }
    if (!sequence) {
      return std::nullopt;
    }
    const std::size_t count = nb::len(raw_values);
    if ((lookup->lookup == "in" && count == 0) ||
        (lookup->lookup != "in" && count != 1)) {
      return std::nullopt;
    }
    node.values.reserve(count);
    nb::sequence raw_sequence = nb::cast<nb::sequence>(raw_values);
    for (nb::handle raw : raw_sequence) {
      auto param = direct_param_from_python(raw, *lookup->field);
      if (!param) {
        return std::nullopt;
      }
      node.values.push_back(std::move(*param));
    }
    return node;
  }

  if (kind == "and") {
    node.kind = 0;
  } else if (kind == "or") {
    node.kind = 1;
  } else if (kind == "xor") {
    node.kind = 4;
  } else if (kind == "not") {
    node.kind = 2;
  } else {
    return std::nullopt;
  }
  if (!tree.contains("children")) {
    return std::nullopt;
  }
  nb::handle raw_children = tree["children"];
  if (!nb::isinstance<nb::list>(raw_children) &&
      !nb::isinstance<nb::tuple>(raw_children)) {
    return std::nullopt;
  }
  const std::size_t count = nb::len(raw_children);
  if (count == 0 || (node.kind == 2 && count != 1)) {
    return std::nullopt;
  }
  node.children.reserve(count);
  nb::sequence children = nb::cast<nb::sequence>(raw_children);
  for (nb::handle raw : children) {
    auto child = native_q_node_from_python(
        model, raw, node.kind == 2 ? !negated : negated);
    if (!child) {
      return std::nullopt;
    }
    node.children.push_back(std::move(*child));
  }
  return node;
}

std::optional<django::orm::QNode> native_q_node_from_python(
    std::uint32_t model_id, nb::handle tree) {
  const auto* model = registered_model(model_id);
  if (!model) {
    return std::nullopt;
  }
  return native_q_node_from_python(*model, tree);
}

std::optional<std::vector<django::orm::ParamValue>>
native_lookup_values_from_python(
    const django::orm::ModelSchema& model, std::string_view field_name,
    bool lookup_in, nb::sequence values) {
  const auto* field = registered_native_field(model, field_name);
  const std::size_t count = nb::len(values);
  if (!field || count == 0 || (!lookup_in && count != 1)) {
    return std::nullopt;
  }
  std::vector<django::orm::ParamValue> out;
  out.reserve(count);
  for (nb::handle raw : values) {
    auto param = direct_param_from_python(raw, *field);
    if (!param) {
      return std::nullopt;
    }
    out.push_back(std::move(*param));
  }
  return out;
}

struct NativeUpdate {
  std::string name;
  django::orm::ParamValue value;
  bool set_null = false;
};

std::optional<std::vector<NativeUpdate>> native_updates_from_python(
    const django::orm::ModelSchema& model, nb::dict updates) {
  if (nb::len(updates) == 0) {
    return std::nullopt;
  }
  std::vector<NativeUpdate> out;
  out.reserve(nb::len(updates));
  for (auto item : updates) {
    if (!PyUnicode_CheckExact(item.first.ptr())) {
      return std::nullopt;
    }
    NativeUpdate update;
    update.name = nb::cast<std::string>(item.first);
    if (update.name.find("__") != std::string::npos) {
      return std::nullopt;
    }
    const auto* field = registered_native_field(model, update.name);
    if (!field) {
      return std::nullopt;
    }
    update.set_null = item.second.is_none();
    auto param = direct_param_from_python(
        item.second, *field, update.set_null && field->nullable);
    if (!param) {
      return std::nullopt;
    }
    update.value = std::move(*param);
    out.push_back(std::move(update));
  }
  return out;
}

std::optional<std::vector<NativeUpdate>> native_updates_from_python(
    const django::orm::ModelSchema& model,
    const std::vector<std::string>& names, nb::sequence values) {
  if (names.empty() || names.size() != nb::len(values)) {
    return std::nullopt;
  }
  std::vector<NativeUpdate> out;
  out.reserve(names.size());
  std::size_t index = 0;
  for (nb::handle raw : values) {
    NativeUpdate update;
    update.name = names[index++];
    if (update.name.find("__") != std::string::npos) {
      return std::nullopt;
    }
    const auto* field = registered_native_field(model, update.name);
    if (!field) {
      return std::nullopt;
    }
    update.set_null = raw.is_none();
    auto param = direct_param_from_python(
        raw, *field, update.set_null && field->nullable);
    if (!param) {
      return std::nullopt;
    }
    update.value = std::move(*param);
    out.push_back(std::move(update));
  }
  return out;
}

std::vector<std::string> update_cache_fields(
    const std::vector<NativeUpdate>& updates) {
  std::vector<std::string> out;
  out.reserve(updates.size());
  for (const auto& update : updates) {
    out.push_back(update.name + (update.set_null ? "#null" : "#value"));
  }
  return out;
}

nb::tuple compile_to_tuple(
    const django::orm::QuerySet& self,
    nb::handle postgres_integer_types = nb::none()) {
  auto compiled = self.compile();
  nb::list params;
  const auto& q = self.query();
  for (auto idx : compiled.param_order) {
    if (idx >= q.params.size()) {
      throw std::runtime_error("param index out of range");
    }
    params.append(param_to_python(q.params[idx], postgres_integer_types));
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

nb::dict q_node_to_python(const django::orm::QNode& node) {
  nb::dict out;
  if (node.kind == 3) {
    out["kind"] = "atom";
    out["key"] = node.key;
    nb::list values;
    for (const auto& value : node.values) {
      values.append(param_to_python(value));
    }
    out["values"] = values;
    if (node.has_rhs_sql) {
      out["rhs_sql"] = node.rhs_sql;
      out["rhs_op"] = node.rhs_op;
      nb::list params;
      for (const auto& value : node.rhs_params) {
        params.append(param_to_python(value));
      }
      out["rhs_params"] = params;
    }
    return out;
  }

  const char* kind = "and";
  if (node.kind == 1) {
    kind = "or";
  } else if (node.kind == 2) {
    kind = "not";
  } else if (node.kind == 4) {
    kind = "xor";
  }
  out["kind"] = kind;
  nb::list children;
  for (const auto& child : node.children) {
    children.append(q_node_to_python(child));
  }
  out["children"] = children;
  return out;
}

}  // namespace

void register_orm_engine(nb::module_& parent) {
  nb::module_ m = parent.def_submodule("orm", "Native ORM data plane");

  m.def("clear_schema", []() {
    django::orm::SchemaRegistry::instance().clear();
    clear_simple_compile_cache();
  });

  m.def(
      "simple_compile_cache_info",
      []() {
        nb::dict out;
        std::lock_guard lock(g_simple_compile_cache_mutex);
        out["size"] = g_simple_compile_cache.size();
        out["hits"] = g_simple_compile_cache_hits.load();
        out["misses"] = g_simple_compile_cache_misses.load();
        return out;
      });

  m.def("clear_simple_compile_cache", &clear_simple_compile_cache);

  m.def(
      "register_model",
      [](const std::string& label, const std::string& db_table, nb::list fields) {
        // fields tuple (min 6, full 16):
        // 0 name, 1 attname, 2 column, 3 class_name, 4 pk, 5 null,
        // 6 remote_table, 7 remote_pk, 8 remote_label,
        // 9 rel_kind ("fk"|"rev_fk"|"m2m"|"rev_m2m"|""),
        // 10 m2m_table, 11 m2m_column, 12 m2m_reverse_column,
        // 13 remote_fk_column, 14 native_direct, 15 generated.
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
          if (n >= 15) {
            f.native_direct = nb::cast<bool>(t[14]);
          } else {
            // Compatibility for the low-level test/debug surface. Production
            // schema exports always send the explicit exact-class bit.
            f.native_direct =
                django::orm::field_type_has_direct_primitive_prep(f.type);
          }
          if (n >= 16) {
            f.generated = nb::cast<bool>(t[15]);
          }
          if (f.rel != django::orm::RelKind::None) {
            f.type = django::orm::FieldType::ForeignKey;
            f.native_direct = false;
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

  nb::class_<django::orm::QueryPlan>(m, "QueryPlan")
      .def_static(
          "for_simple_filter",
          [](std::uint32_t model_id, const std::string& lookup_field,
             bool lookup_in, nb::sequence lookup_values) -> nb::object {
            const auto* model = registered_model(model_id);
            if (!model || lookup_field.find("__") != std::string::npos) {
              return nb::none();
            }
            auto values = native_lookup_values_from_python(
                *model, lookup_field, lookup_in, lookup_values);
            if (!values) {
              return nb::none();
            }
            return nb::cast(
                django::orm::QueryPlan(
                    static_cast<django::orm::ModelId>(model_id))
                    .with_simple_filter(
                        lookup_field, lookup_in, std::move(*values)));
          },
          nb::arg("model_id"), nb::arg("lookup_field"),
          nb::arg("lookup_in"), nb::arg("lookup_values"))
      .def_static(
          "for_q",
          [](std::uint32_t model_id, nb::dict tree) -> nb::object {
            auto node = native_q_node_from_python(model_id, tree);
            if (!node) {
              return nb::none();
            }
            return nb::cast(
                django::orm::QueryPlan(
                    static_cast<django::orm::ModelId>(model_id))
                    .with_filter(std::move(*node)));
          },
          nb::arg("model_id"), nb::arg("tree"))
      .def_static(
          "for_ordering",
          [](std::uint32_t model_id, nb::sequence names) -> nb::object {
            const auto* model = registered_model(model_id);
            auto ordering = strings_from_sequence(names);
            if (!model || !validate_native_ordering(*model, ordering)) {
              return nb::none();
            }
            return nb::cast(
                django::orm::QueryPlan(
                    static_cast<django::orm::ModelId>(model_id))
                    .with_ordering(std::move(ordering)));
          },
          nb::arg("model_id"), nb::arg("names"))
      .def_static(
          "for_values",
          [](std::uint32_t model_id, nb::sequence names) -> nb::object {
            const auto* model = registered_model(model_id);
            auto fields = strings_from_sequence(names);
            if (!model || !validate_native_projection(*model, fields)) {
              return nb::none();
            }
            return nb::cast(
                django::orm::QueryPlan(
                    static_cast<django::orm::ModelId>(model_id))
                    .with_values(std::move(fields)));
          },
          nb::arg("model_id"), nb::arg("names"))
      .def_static(
          "from_simple_filter",
          [](const std::string& lookup_field, bool lookup_in,
             nb::sequence lookup_values) {
            std::vector<django::orm::ParamValue> values;
            values.reserve(nb::len(lookup_values));
            for (nb::handle value : lookup_values) {
              values.push_back(param_from_python(value));
            }
            return django::orm::QueryPlan{}.with_simple_filter(
                lookup_field, lookup_in, std::move(values));
          },
          nb::arg("lookup_field"), nb::arg("lookup_in"),
          nb::arg("lookup_values"))
      .def_static(
          "from_q",
          [](nb::dict tree) {
            return django::orm::QueryPlan{}.with_filter(
                q_node_from_python(tree));
          },
          nb::arg("tree"))
      .def_static(
          "from_ordering",
          [](nb::sequence names) {
            return django::orm::QueryPlan{}.with_ordering(
                strings_from_sequence(names));
          },
          nb::arg("names"))
      .def_static(
          "from_values",
          [](nb::sequence names) {
            return django::orm::QueryPlan{}.with_values(
                strings_from_sequence(names));
          },
          nb::arg("names"))
      .def(
          "with_q",
          [](const django::orm::QueryPlan& self, nb::dict tree) -> nb::object {
            if (!self.is_model_bound()) {
              return nb::cast(self.with_filter(q_node_from_python(tree)));
            }
            auto model_id = self.model_id();
            if (!model_id || !self.matches_model(*model_id)) {
              return nb::none();
            }
            auto node = native_q_node_from_python(
                static_cast<std::uint32_t>(*model_id), tree);
            if (!node) {
              return nb::none();
            }
            return nb::cast(self.with_filter(std::move(*node)));
          },
          nb::arg("tree"))
      .def(
          "with_ordering",
          [](const django::orm::QueryPlan& self,
             nb::sequence names) -> nb::object {
            auto ordering = strings_from_sequence(names);
            if (!self.is_model_bound()) {
              return nb::cast(self.with_ordering(std::move(ordering)));
            }
            auto model_id = self.model_id();
            const auto* model = model_id ? registered_model(*model_id) : nullptr;
            if (!model_id || !self.matches_model(*model_id) || !model ||
                !validate_native_ordering(*model, ordering)) {
              return nb::none();
            }
            return nb::cast(self.with_ordering(std::move(ordering)));
          },
          nb::arg("names"))
      .def(
          "with_values",
          [](const django::orm::QueryPlan& self,
             nb::sequence names) -> nb::object {
            auto fields = strings_from_sequence(names);
            if (!self.is_model_bound()) {
              return nb::cast(self.with_values(std::move(fields)));
            }
            auto model_id = self.model_id();
            const auto* model = model_id ? registered_model(*model_id) : nullptr;
            if (!model_id || !self.matches_model(*model_id) || !model ||
                !validate_native_projection(*model, fields)) {
              return nb::none();
            }
            return nb::cast(self.with_values(std::move(fields)));
          },
          nb::arg("names"))
      .def("has_only_projection",
           &django::orm::QueryPlan::has_only_projection)
      .def("has_simple_filter", &django::orm::QueryPlan::has_simple_filter)
      .def(
          "simple_filter",
          [](const django::orm::QueryPlan& self) -> nb::object {
            for (const auto& operation : self.operations()) {
              if (operation.kind !=
                      django::orm::QueryPlan::OperationKind::Filter ||
                  !operation.simple_filter) {
                continue;
              }
              nb::list values;
              for (const auto& value : operation.filter.values) {
                values.append(param_to_python(value));
              }
              return nb::make_tuple(operation.lookup_field,
                                    operation.lookup_in, values);
            }
            return nb::none();
          })
      .def(
          "replay",
          [](const django::orm::QueryPlan& self) {
            nb::list replay;
            for (const auto& operation : self.operations()) {
              switch (operation.kind) {
                case django::orm::QueryPlan::OperationKind::Filter:
                  replay.append(nb::make_tuple(
                      "filter", q_node_to_python(operation.filter)));
                  break;
                case django::orm::QueryPlan::OperationKind::OrderBy:
                  replay.append(nb::make_tuple(
                      "order_by",
                      django::native::list_from_strings(operation.names)));
                  break;
                case django::orm::QueryPlan::OperationKind::Values:
                  replay.append(nb::make_tuple(
                      "values",
                      django::native::list_from_strings(operation.names)));
                  break;
              }
            }
            return replay;
          });

  m.def(
      "compile_simple_values_get",
      [](std::uint32_t model_id, int dialect, nb::sequence field_names,
         const std::string& lookup_field, nb::handle lookup_value,
         std::uint64_t limit,
         nb::handle postgres_integer_types) -> nb::object {
        auto fields = strings_from_sequence(field_names);
        const auto* model = registered_model(model_id);
        auto lookup = model
                          ? registered_native_lookup(*model, lookup_field)
                          : std::optional<NativeLookup>{};
        if (!model || !validate_native_projection(*model, fields) || !lookup ||
            lookup->lookup != "exact") {
          return nb::none();
        }
        const bool null_lookup = lookup_value.is_none();
        auto prepared = direct_param_from_python(
            lookup_value, *lookup->field, null_lookup);
        if (!prepared) {
          return nb::none();
        }
        const auto key = simple_compile_key(
            'G', model_id, dialect, fields, lookup_field, {}, limit, 0,
            null_lookup);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.values(fields, true, django::orm::ResultMode::ValuesList)) {
            return std::string{};
          }
          const bool filtered =
              null_lookup
                  ? qs.filter_isnull(lookup_field, true)
                  : qs.filter_eq(lookup_field,
                                 django::orm::ParamValue::from_int(0));
          if (!filtered) {
            return std::string{};
          }
          qs.set_limit(limit);
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        nb::list params;
        if (!null_lookup) {
          params.append(param_to_python(*prepared, postgres_integer_types));
        }
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), params);
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("field_names"),
      nb::arg("lookup_field"), nb::arg("lookup_value"), nb::arg("limit"),
      nb::arg("postgres_integer_types") = nb::none());

  m.def(
      "compile_simple_values_select",
      [](std::uint32_t model_id, int dialect, nb::sequence field_names,
         nb::sequence ordering_names, std::uint64_t limit,
         std::uint64_t offset) -> nb::object {
        auto fields = strings_from_sequence(field_names);
        auto ordering = strings_from_sequence(ordering_names);
        const auto* model = registered_model(model_id);
        if (!model || !validate_native_projection(*model, fields) ||
            !validate_native_ordering(*model, ordering)) {
          return nb::none();
        }
        const auto key = simple_compile_key(
            'S', model_id, dialect, fields, {}, ordering, limit, offset);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.values(fields, true, django::orm::ResultMode::ValuesList)) {
            return std::string{};
          }
          for (const auto& item : ordering) {
            bool desc = !item.empty() && item.front() == '-';
            std::string_view name(item);
            if (desc) {
              name.remove_prefix(1);
            }
            if (name.empty() || name == "?" || !qs.order_by(name, desc)) {
              return std::string{};
            }
          }
          if (limit > 0) {
            qs.set_limit(limit);
          }
          if (offset > 0) {
            qs.set_offset(offset);
          }
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), nb::list());
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("field_names"),
      nb::arg("ordering_names"), nb::arg("limit") = 0,
      nb::arg("offset") = 0);

  m.def(
      "compile_simple_values_filter",
      [](std::uint32_t model_id, int dialect, nb::sequence field_names,
         const std::string& lookup_field, bool lookup_in,
         nb::sequence lookup_values, nb::sequence ordering_names,
         std::uint64_t limit, std::uint64_t offset,
         nb::handle postgres_integer_types) -> nb::object {
        auto fields = strings_from_sequence(field_names);
        auto ordering = strings_from_sequence(ordering_names);
        const auto* model = registered_model(model_id);
        const std::size_t value_count = nb::len(lookup_values);
        auto prepared = model ? native_lookup_values_from_python(
                                    *model, lookup_field, lookup_in,
                                    lookup_values)
                              : std::nullopt;
        if (!model || !validate_native_projection(*model, fields) ||
            !validate_native_ordering(*model, ordering) || !prepared ||
            value_count == 0 || (!lookup_in && value_count != 1)) {
          return nb::none();
        }
        std::string lookup_key = lookup_field;
        lookup_key += lookup_in ? "__in#" : "__exact#";
        lookup_key += std::to_string(value_count);
        const auto key = simple_compile_key(
            'F', model_id, dialect, fields, lookup_key, ordering, limit, offset);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.values(fields, true, django::orm::ResultMode::ValuesList)) {
            return std::string{};
          }
          bool filtered = false;
          if (lookup_in) {
            std::vector<django::orm::ParamValue> values;
            values.reserve(value_count);
            for (std::size_t i = 0; i < value_count; ++i) {
              values.push_back(django::orm::ParamValue::from_int(0));
            }
            filtered = qs.filter_in(lookup_field, std::move(values));
          } else {
            filtered = qs.filter_eq(lookup_field,
                                    django::orm::ParamValue::from_int(0));
          }
          if (!filtered) {
            return std::string{};
          }
          for (const auto& item : ordering) {
            const bool desc = !item.empty() && item.front() == '-';
            std::string_view name(item);
            if (desc) {
              name.remove_prefix(1);
            }
            if (name.empty() || name == "?" || !qs.order_by(name, desc)) {
              return std::string{};
            }
          }
          if (limit > 0) {
            qs.set_limit(limit);
          }
          if (offset > 0) {
            qs.set_offset(offset);
          }
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        nb::list params;
        for (const auto& value : *prepared) {
          params.append(param_to_python(value, postgres_integer_types));
        }
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), params);
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("field_names"),
      nb::arg("lookup_field"), nb::arg("lookup_in"),
      nb::arg("lookup_values"), nb::arg("ordering_names") = nb::tuple(),
      nb::arg("limit") = 0, nb::arg("offset") = 0,
      nb::arg("postgres_integer_types") = nb::none());

  m.def(
      "compile_simple_values_plan",
      [](std::uint32_t model_id, int dialect,
         const django::orm::QueryPlan& plan, std::uint64_t limit,
         std::uint64_t offset,
         nb::handle postgres_integer_types) -> nb::object {
        auto shape = plan.simple_values_shape();
        if (!shape || !plan.matches_model(
                          static_cast<django::orm::ModelId>(model_id))) {
          return nb::none();
        }
        std::string lookup_key = shape->lookup_field;
        lookup_key += shape->lookup_in ? "__in#" : "__exact#";
        lookup_key += std::to_string(shape->lookup_values->size());
        const auto key = simple_compile_key(
            'F', model_id, dialect, shape->fields, lookup_key,
            shape->ordering, limit, offset);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.values(shape->fields, true,
                         django::orm::ResultMode::ValuesList)) {
            return std::string{};
          }
          bool filtered = false;
          if (shape->lookup_in) {
            std::vector<django::orm::ParamValue> values;
            values.reserve(shape->lookup_values->size());
            for (std::size_t i = 0; i < shape->lookup_values->size(); ++i) {
              values.push_back(django::orm::ParamValue::from_int(0));
            }
            filtered = qs.filter_in(shape->lookup_field, std::move(values));
          } else {
            filtered = qs.filter_eq(shape->lookup_field,
                                    django::orm::ParamValue::from_int(0));
          }
          if (!filtered) {
            return std::string{};
          }
          for (const auto& item : shape->ordering) {
            const bool desc = !item.empty() && item.front() == '-';
            std::string_view name(item);
            if (desc) {
              name.remove_prefix(1);
            }
            if (name.empty() || name == "?" || !qs.order_by(name, desc)) {
              return std::string{};
            }
          }
          if (limit > 0) {
            qs.set_limit(limit);
          }
          if (offset > 0) {
            qs.set_offset(offset);
          }
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        nb::list params;
        for (const auto& value : *shape->lookup_values) {
          params.append(param_to_python(value, postgres_integer_types));
        }
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), params);
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("plan"),
      nb::arg("limit") = 0, nb::arg("offset") = 0,
      nb::arg("postgres_integer_types") = nb::none());

  m.def(
      "compile_simple_update",
      [](std::uint32_t model_id, int dialect,
         const std::string& lookup_field, nb::handle lookup_value,
         nb::sequence update_names, nb::sequence update_values,
         nb::handle postgres_integer_types) -> nb::object {
        auto fields = strings_from_sequence(update_names);
        const auto* model = registered_model(model_id);
        auto lookup = model
                          ? registered_native_lookup(*model, lookup_field)
                          : std::optional<NativeLookup>{};
        auto prepared_lookup = lookup
                                   ? direct_param_from_python(
                                         lookup_value, *lookup->field)
                                   : std::nullopt;
        auto updates = model ? native_updates_from_python(
                                   *model, fields, update_values)
                             : std::nullopt;
        if (!model || !lookup || lookup->lookup != "exact" ||
            !prepared_lookup || !updates) {
          return nb::none();
        }
        const auto cache_fields = update_cache_fields(*updates);
        const auto key = simple_compile_key(
            'U', model_id, dialect, cache_fields, lookup_field, {}, 0, 0);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.filter_eq(lookup_field,
                            django::orm::ParamValue::from_int(0))) {
            return std::string{};
          }
          for (const auto& update : *updates) {
            const bool added =
                update.set_null
                    ? qs.add_update_null(update.name)
                    : qs.add_update(
                          update.name,
                          django::orm::ParamValue::from_int(0));
            if (!added) {
              return std::string{};
            }
          }
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        nb::list params;
        for (const auto& update : *updates) {
          if (!update.set_null) {
            params.append(
                param_to_python(update.value, postgres_integer_types));
          }
        }
        params.append(
            param_to_python(*prepared_lookup, postgres_integer_types));
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), params);
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("lookup_field"),
      nb::arg("lookup_value"), nb::arg("update_names"),
      nb::arg("update_values"),
      nb::arg("postgres_integer_types") = nb::none());

  m.def(
      "compile_simple_update_plan",
      [](std::uint32_t model_id, int dialect,
         const django::orm::QueryPlan& plan, nb::dict raw_updates,
         nb::handle postgres_integer_types) -> nb::object {
        auto shape = plan.simple_update_shape();
        const auto* model = registered_model(model_id);
        auto updates = model
                           ? native_updates_from_python(*model, raw_updates)
                           : std::nullopt;
        if (!shape || !model || !updates ||
            !plan.matches_model(
                static_cast<django::orm::ModelId>(model_id))) {
          return nb::none();
        }
        const auto cache_fields = update_cache_fields(*updates);
        const auto key = simple_compile_key(
            'U', model_id, dialect, cache_fields, shape->lookup_field, {}, 0, 0);
        auto sql = cached_simple_sql(key, [&]() {
          django::orm::QuerySet qs(
              static_cast<django::orm::ModelId>(model_id),
              static_cast<django::orm::DialectId>(dialect));
          if (!qs.filter_eq(shape->lookup_field,
                            django::orm::ParamValue::from_int(0))) {
            return std::string{};
          }
          for (const auto& update : *updates) {
            const bool added =
                update.set_null
                    ? qs.add_update_null(update.name)
                    : qs.add_update(
                          update.name,
                          django::orm::ParamValue::from_int(0));
            if (!added) {
              return std::string{};
            }
          }
          return qs.compile().sql;
        });
        if (sql.empty()) {
          return nb::none();
        }
        nb::list params;
        for (const auto& update : *updates) {
          if (!update.set_null) {
            params.append(
                param_to_python(update.value, postgres_integer_types));
          }
        }
        params.append(
            param_to_python(*shape->lookup_value, postgres_integer_types));
        return nb::make_tuple(nb::str(sql.c_str(), sql.size()), params);
      },
      nb::arg("model_id"), nb::arg("dialect"), nb::arg("plan"),
      nb::arg("updates"),
      nb::arg("postgres_integer_types") = nb::none());

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
      .def_static(
          "create_from_q",
          [](std::uint32_t model_id, int dialect, nb::dict tree) -> nb::object {
            django::orm::QuerySet qs(
                static_cast<django::orm::ModelId>(model_id),
                static_cast<django::orm::DialectId>(dialect));
            if (!qs.apply_q(q_node_from_python(tree))) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
          },
          nb::arg("model_id"), nb::arg("dialect"), nb::arg("tree"))
      .def_static(
          "create_from_native_q",
          [](std::uint32_t model_id, int dialect,
             nb::dict tree) -> nb::object {
            auto node = native_q_node_from_python(model_id, tree);
            if (!node) {
              return nb::none();
            }
            django::orm::QuerySet qs(
                static_cast<django::orm::ModelId>(model_id),
                static_cast<django::orm::DialectId>(dialect));
            if (!qs.apply_q(*node)) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
          },
          nb::arg("model_id"), nb::arg("dialect"), nb::arg("tree"))
      .def("clone", &django::orm::QuerySet::clone)
      .def("shares_state_with", &django::orm::QuerySet::shares_state_with,
           nb::arg("other"))
      .def("compile_runs", &django::orm::QuerySet::compile_runs)
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
          "with_q",
          [](const django::orm::QuerySet& self, nb::dict tree) -> nb::object {
            django::orm::QuerySet qs = self.clone();
            if (!qs.apply_q(q_node_from_python(tree))) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
          },
          nb::arg("tree"),
          "Return a COW clone with a Q-tree applied.")
      .def(
          "with_native_q",
          [](const django::orm::QuerySet& self,
             nb::dict tree) -> nb::object {
            auto node = native_q_node_from_python(self.model_id(), tree);
            if (!node) {
              return nb::none();
            }
            django::orm::QuerySet qs = self.clone();
            if (!qs.apply_q(*node)) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
          },
          nb::arg("tree"),
          "Return a COW clone with a schema-validated primitive Q-tree.")
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
          "with_values_list",
          [](const django::orm::QuerySet& self, nb::sequence names,
             bool flat) -> nb::object {
            django::orm::QuerySet qs = self.clone();
            auto ns = strings_from_sequence(names);
            auto mode = flat ? django::orm::ResultMode::ValuesListFlat
                             : django::orm::ResultMode::ValuesList;
            if (!qs.values(ns, true, mode)) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
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
          "with_values",
          [](const django::orm::QuerySet& self,
             nb::sequence names) -> nb::object {
            django::orm::QuerySet qs = self.clone();
            auto ns = strings_from_sequence(names);
            if (!qs.values(ns, true, django::orm::ResultMode::ValuesDict)) {
              return nb::none();
            }
            return nb::cast(std::move(qs));
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
      .def(
          "set_ordering",
          [](django::orm::QuerySet& self, nb::sequence names) {
            self.clear_ordering();
            for (nb::handle value : names) {
              std::string item = nb::cast<std::string>(value);
              bool desc = !item.empty() && item.front() == '-';
              std::string_view name(item);
              if (desc) {
                name.remove_prefix(1);
              }
              if (name.empty() || name == "?" || !self.order_by(name, desc)) {
                return false;
              }
            }
            return true;
          },
          nb::arg("names"))
      .def(
          "with_ordering",
          [](const django::orm::QuerySet& self,
             nb::sequence names) -> nb::object {
            django::orm::QuerySet qs = self.clone();
            qs.clear_ordering();
            for (nb::handle value : names) {
              std::string item = nb::cast<std::string>(value);
              bool desc = !item.empty() && item.front() == '-';
              std::string_view name(item);
              if (desc) {
                name.remove_prefix(1);
              }
              if (name.empty() || name == "?" || !qs.order_by(name, desc)) {
                return nb::none();
              }
            }
            return nb::cast(std::move(qs));
          },
          nb::arg("names"))
      .def(
          "compile_update_kwargs",
          [](const django::orm::QuerySet& self, nb::dict kwargs,
             nb::handle postgres_integer_types) -> nb::object {
            const auto* model = registered_model(self.model_id());
            auto updates = model
                               ? native_updates_from_python(*model, kwargs)
                               : std::nullopt;
            if (!updates) {
              return nb::none();
            }
            django::orm::QuerySet qs = self.clone();
            for (const auto& update : *updates) {
              const bool added = update.set_null
                                     ? qs.add_update_null(update.name)
                                     : qs.add_update(update.name, update.value);
              if (!added) {
                return nb::none();
              }
            }
            if (qs.compile().sql.empty()) {
              return nb::none();
            }
            return compile_to_tuple(qs, postgres_integer_types);
          },
          nb::arg("kwargs"),
          nb::arg("postgres_integer_types") = nb::none())
      .def("set_limit", &django::orm::QuerySet::set_limit, nb::arg("n"))
      .def("set_offset", &django::orm::QuerySet::set_offset, nb::arg("n"))
      .def("set_distinct", &django::orm::QuerySet::set_distinct, nb::arg("v"))
      .def("compile_sql", &compile_to_tuple,
           nb::arg("postgres_integer_types") = nb::none())
      .def("base_attnames",
           [](const django::orm::QuerySet& self) {
             return django::native::list_from_strings(self.base_attnames());
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
