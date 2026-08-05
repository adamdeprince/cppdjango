// Schema snapshot registry: Python Meta → C++ (setup path, not per-filter).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace django::orm {

using ModelId = std::uint32_t;
using FieldId = std::uint32_t;

enum class FieldType : std::uint8_t {
  Integer = 0,
  BigInteger,
  SmallInteger,
  Auto,
  BigAuto,
  Float,
  Decimal,
  Boolean,
  Text,
  Char,
  Date,
  DateTime,
  Time,
  UUID,
  Binary,
  Other,
};

struct FieldSchema {
  FieldId id = 0;
  std::string name;      // Python field name / attname for lookups
  std::string attname;   // instance attribute
  std::string column;    // DB column
  FieldType type = FieldType::Other;
  bool primary_key = false;
  bool nullable = false;
};

struct ModelSchema {
  ModelId id = 0;
  std::string label;  // app_label.ModelName
  std::string db_table;
  std::vector<FieldSchema> fields;
  FieldId pk_field = 0;
  // name / attname / "pk" → field id
  std::unordered_map<std::string, FieldId> field_by_name;
};

class SchemaRegistry {
 public:
  static SchemaRegistry& instance();

  // Register or replace by label. Returns model id.
  ModelId register_model(ModelSchema schema);

  [[nodiscard]] const ModelSchema* get(ModelId id) const;
  [[nodiscard]] const ModelSchema* get_by_label(std::string_view label) const;
  [[nodiscard]] std::optional<ModelId> find_id(std::string_view label) const;

  void clear();  // tests

 private:
  SchemaRegistry() = default;
  std::vector<ModelSchema> models_;
  std::unordered_map<std::string, ModelId> by_label_;
};

// Map Django field class name → FieldType (best-effort).
[[nodiscard]] FieldType field_type_from_class_name(std::string_view class_name);

}  // namespace django::orm
